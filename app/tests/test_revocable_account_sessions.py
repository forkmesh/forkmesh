"""Account sessions are random, revocable, cookie-safe, and non-replayable."""

import ast
import asyncio
import hashlib
import hmac
from pathlib import Path
import re
import sqlite3
from types import SimpleNamespace
from urllib.parse import urlparse
from _maintenance_stubs import with_maintenance_off


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
TEXT = ENTRY.read_text(encoding="utf-8")


def _source(name):
    tree = ast.parse(TEXT)
    for node in tree.body:
        if (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name == name
        ):
            return ast.get_source_segment(TEXT, node)
    raise AssertionError(name)


def _lookup_namespace(row, token, now=2_000_000):
    updates = []

    async def first(_env, sql, *_args):
        if "FROM account_sessions" in sql:
            return dict(row) if row else None
        if "FROM users" in sql:
            return {"data": {"name": "alice", "status": "active", "kind": "user"}}
        raise AssertionError(sql)

    async def run(_env, sql, *args):
        updates.append((sql, args))

    async def first_primary(_env, sql, *args):
        return await first(_env, sql, *args)

    namespace = {
        "ACCOUNT_SESSION_TOKEN_RE": re.compile(
            r"^v2\.([A-Za-z0-9_-]{24,64})\.([A-Za-z0-9_-]{32,96})$"),
        "ACCOUNT_SESSION_TOUCH_MS": 300_000,
        "ACCOUNT_SESSION_TTL_MS": 1826 * 24 * 60 * 60 * 1000,
        "Date": SimpleNamespace(now=lambda: now),
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "d1_first": first,
        "d1_first_primary": first_primary,
        "d1_run": run,
        "decrypt_row": lambda _env, value: asyncio.sleep(0, result=value),
        "_account_kind": lambda rec: rec.get("kind"),
        "_account_session_token_digest": lambda _env, value: hmac.new(
            b"server-secret",
            ("forkmesh-account-session-token-v2\n" + value).encode(),
            "sha256",
        ).hexdigest(),
        "hmac": hmac,
    }
    exec(_source("_account_session_lookup"), with_maintenance_off(namespace))
    return namespace["_account_session_lookup"], updates


def _record_namespace(row_overrides=None, user_overrides=None, now=2_000_000):
    """Load the real session record/lookup stack around an in-memory D1."""
    token = "v2." + "a" * 32 + "." + "b" * 43
    env = SimpleNamespace(ADMIN_PASS="admin-secret", DATA_KEY="data-secret")
    session_row = {}
    user_row = {
        "name": "alice",
        "status": "active",
        "kind": "user",
        "pass_hash": "hash",
    }
    user_row.update(user_overrides or {})
    queries = []
    updates = []

    async def first(_env, sql, *args):
        queries.append((sql, args))
        if "FROM account_sessions" in sql:
            return dict(session_row) if session_row else None
        if "FROM users" in sql:
            return {"data": dict(user_row)}
        raise AssertionError(sql)

    async def run(_env, sql, *args):
        updates.append((sql, args))

    async def decrypt(_env, value):
        return dict(value) if isinstance(value, dict) else None

    namespace = {
        "ACCOUNT_SESSION_COOKIE": "forkmesh_account",
        "ACCOUNT_SESSION_TOKEN_RE": re.compile(
            r"^v2\.([A-Za-z0-9_-]{24,64})\.([A-Za-z0-9_-]{32,96})$"),
        "ACCOUNT_SESSION_TOUCH_MS": 300_000,
        "ACCOUNT_SESSION_TTL_MS": 1826 * 24 * 60 * 60 * 1000,
        "ADMIN_SESSION_TTL_MS": 12 * 60 * 60 * 1000,
        "MAX_NODE_NAME": 63,
        # The unverified-account gate the record helper now applies; the
        # fixture account is verified, so it never fires here. Its own contract
        # lives in test_unverified_account_gate.
        "UNVERIFIED_SAFE_METHODS": ("GET", "HEAD", "OPTIONS"),
        "UNVERIFIED_EMAIL_ERROR": "email_not_verified",
        "Date": SimpleNamespace(now=lambda: now),
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "d1_first": first,
        "d1_first_primary": first,
        "d1_run": run,
        "decrypt_row": decrypt,
        "hmac": hmac,
        "re": re,
        "urlparse": urlparse,
    }
    namespace["_request_origin_allowed"] = (
        lambda _env, request: namespace["_request_same_origin"](request)
    )
    for name in (
        "method_name",
        "_account_kind",
        "_account_session_secret",
        "_account_session_token_digest",
        "_cookie_value",
        "_request_account_session_token",
        "_request_same_origin",
        "_account_session_lookup",
        "_account_email_unverified",
        "_account_session_record",
    ):
        exec(_source(name), with_maintenance_off(namespace))

    session_row.update({
        "account_bi": "account-bi",
        "token_digest": namespace["_account_session_token_digest"](env, token),
        "last_seen_at": 0,
        "expires_at": now + 1_000_000,
        "revoked_at": 0,
    })
    session_row.update(row_overrides or {})
    return namespace["_account_session_record"], token, env, queries, updates


def _session_request(method="GET", headers=None):
    return SimpleNamespace(
        method=method,
        url="https://forkmesh.com/api/profile",
        headers=dict(headers or {}),
    )


def test_session_lookup_rejects_revoked_expired_and_wrong_digest():
    token = "v2." + "a" * 32 + "." + "b" * 43
    digest = hmac.new(
        b"server-secret",
        ("forkmesh-account-session-token-v2\n" + token).encode(),
        "sha256",
    ).hexdigest()
    base = {
        "account_bi": "account-bi",
        "token_digest": digest,
        "last_seen_at": 0,
        "expires_at": 3_000_000,
        "revoked_at": 0,
    }
    lookup, updates = _lookup_namespace(base, token)
    account_bi, rec, session_id = asyncio.run(lookup(object(), token))
    assert account_bi == "account-bi"
    assert rec["name"] == "alice"
    assert session_id == "a" * 32
    assert updates and "last_seen_at" in updates[0][0]

    for override in (
        {"revoked_at": 1},
        {"expires_at": 2_000_000},
        {"token_digest": "0" * 64},
    ):
        lookup, _ = _lookup_namespace({**base, **override}, token)
        assert asyncio.run(lookup(object(), token)) == ("", None, "")


def test_account_sessions_last_five_years_and_admin_cookie_stays_short():
    namespace = {}
    for line in TEXT.splitlines():
        if line.startswith(("ACCOUNT_SESSION_TTL_MS =", "ADMIN_SESSION_TTL_MS =")):
            exec(line, with_maintenance_off(namespace))
    day_ms = 24 * 60 * 60 * 1000
    assert namespace["ACCOUNT_SESSION_TTL_MS"] >= 5 * 365 * day_ms
    assert namespace["ADMIN_SESSION_TTL_MS"] == 12 * 60 * 60 * 1000
    mint = _source("_account_session_token")
    assert "expires = now + ACCOUNT_SESSION_TTL_MS" in mint
    assert "ADMIN_SESSION_TTL_MS" not in mint
    # Eviction keeps the most recently used sessions, not the newest sign-ins,
    # so logins elsewhere cannot push out a browser that is still in use.
    assert "ORDER BY last_seen_at DESC" in mint
    cookie_ns = {
        "ACCOUNT_SESSION_COOKIE": "forkmesh_account",
        "ACCOUNT_SESSION_TTL_MS": namespace["ACCOUNT_SESSION_TTL_MS"],
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
    }
    exec(_source("_account_session_cookie"), with_maintenance_off(cookie_ns))
    cookie = cookie_ns["_account_session_cookie"]("v2.token")
    assert "Max-Age=%d;" % (namespace["ACCOUNT_SESSION_TTL_MS"] // 1000) in cookie
    assert "HttpOnly" in cookie and "Secure" in cookie


def test_session_touch_lifts_a_short_legacy_session_to_the_full_lifetime():
    token = "v2." + "a" * 32 + "." + "b" * 43
    digest = hmac.new(
        b"server-secret",
        ("forkmesh-account-session-token-v2\n" + token).encode(),
        "sha256",
    ).hexdigest()
    now = 2_000_000
    ttl = 1826 * 24 * 60 * 60 * 1000
    legacy_expiry = 1_000_000 + 12 * 60 * 60 * 1000
    lookup, updates = _lookup_namespace({
        "account_bi": "account-bi",
        "token_digest": digest,
        "last_seen_at": 0,
        "expires_at": legacy_expiry,
        "revoked_at": 0,
    }, token, now=now)
    asyncio.run(lookup(object(), token))
    assert len(updates) == 1
    sql, args = updates[0]

    # Run the real statement against SQLite so MAX(...) is checked, not text.
    db = sqlite3.connect(":memory:")
    db.execute(
        "CREATE TABLE account_sessions (session_id TEXT, created_at INTEGER,"
        " last_seen_at INTEGER, expires_at INTEGER, revoked_at INTEGER)")
    db.execute(
        "INSERT INTO account_sessions VALUES (?,?,?,?,0)",
        ("a" * 32, 1_000_000, 0, legacy_expiry))
    db.execute(
        "INSERT INTO account_sessions VALUES (?,?,?,?,0)",
        ("z" * 32, 1_000_000, 0, legacy_expiry))
    db.execute(sql, args)
    rows = dict(db.execute(
        "SELECT session_id, expires_at FROM account_sessions").fetchall())
    assert rows["a" * 32] == 1_000_000 + ttl
    assert rows["z" * 32] == legacy_expiry
    # A session already past the full lifetime is never shortened.
    db.execute("UPDATE account_sessions SET expires_at=? WHERE session_id=?",
               (1_000_000 + ttl + 5, "a" * 32))
    db.execute(sql, args)
    assert db.execute(
        "SELECT expires_at FROM account_sessions WHERE session_id=?",
        ("a" * 32,)).fetchone()[0] == 1_000_000 + ttl + 5


def test_primary_session_read_uses_latest_d1_state_when_available():
    source = _source("d1_first_primary")
    calls = []

    class Statement:
        def bind(self, *args):
            calls.append(("bind", args))
            return self

        async def first(self):
            calls.append(("first",))
            return {"session_id": "current"}

    class Database:
        def withSession(self, constraint):
            calls.append(("withSession", constraint))
            return SimpleNamespace(prepare=lambda sql: Statement())

    namespace = {
        "d1_first": lambda *_args: None,
        "_cron_d1_query_charge": lambda: calls.append(("charge",)),
        "_log_d1_query": lambda *_args: None,
        "_is_transient_d1_error": lambda _error: False,
        "d1_bind_args": lambda args: tuple(args),
        "d1_row_to_dict": lambda row: row,
        "getattr": getattr,
    }
    exec(source, with_maintenance_off(namespace))
    result = asyncio.run(namespace["d1_first_primary"](
        SimpleNamespace(DB=Database()),
        "SELECT session_id FROM account_sessions WHERE session_id=?",
        "current",
    ))
    assert result == {"session_id": "current"}
    assert ("withSession", "first-primary") in calls
    assert ("bind", ("current",)) in calls


def test_primary_session_read_falls_back_when_sessions_api_is_unavailable():
    source = _source("d1_first_primary")
    calls = []

    async def fallback(*args):
        calls.append(args)
        return {"session_id": "fallback"}

    namespace = {
        "d1_first": fallback,
        "_cron_d1_query_charge": lambda: None,
        "_is_transient_d1_error": lambda _error: False,
        "d1_bind_args": lambda args: tuple(args),
        "d1_row_to_dict": lambda row: row,
        "getattr": getattr,
    }
    exec(source, with_maintenance_off(namespace))
    result = asyncio.run(namespace["d1_first_primary"](
        SimpleNamespace(DB=SimpleNamespace()), "SELECT 1"))
    assert result == {"session_id": "fallback"}
    assert calls


def test_account_session_record_uses_v2_digest_lookup_for_bearer_tokens():
    record, token, env, queries, updates = _record_namespace()
    request = _session_request(
        headers={"authorization": "Bearer " + token})
    account_bi, account = asyncio.run(record(env, request))
    assert account_bi == "account-bi"
    assert account["name"] == "alice"
    assert queries[0][1] == ("a" * 32,)
    assert any("FROM users" in sql for sql, _args in queries)
    assert updates and "last_seen_at" in updates[0][0]

    # The public session id is not sufficient: changing the secret keeps the
    # lookup key the same but fails the keyed digest comparison.
    forged = "v2." + "a" * 32 + "." + "c" * 43
    assert asyncio.run(record(
        env,
        _session_request(headers={"authorization": "Bearer " + forged}),
    )) == ("", None)


def test_account_session_record_rejects_expired_revoked_and_non_user_accounts():
    cases = (
        ({"expires_at": 2_000_000}, {}, "expired"),
        ({"revoked_at": 1}, {}, "revoked"),
        ({}, {"status": "disabled"}, "inactive"),
        ({}, {"kind": "node", "pass_hash": ""}, "node-kind"),
    )
    for row_overrides, user_overrides, label in cases:
        record, token, env, _queries, _updates = _record_namespace(
            row_overrides=row_overrides,
            user_overrides=user_overrides,
        )
        actual = asyncio.run(record(
            env,
            _session_request(headers={"authorization": "Bearer " + token}),
        ))
        assert actual == ("", None), label


def test_account_session_record_cookie_selection_enforces_mutation_origin():
    record, token, env, _queries, _updates = _record_namespace()
    cookie = "forkmesh_account=" + token

    same_origin = _session_request(
        method="POST",
        headers={
            "cookie": cookie,
            "origin": "https://forkmesh.com",
        },
    )
    account_bi, account = asyncio.run(
        record(env, same_origin, {"sessionToken": "cookie"}))
    assert account_bi == "account-bi"
    assert account["name"] == "alice"

    for origin in ("", "https://mirror.forkmesh.com"):
        denied = _session_request(
            method="POST",
            headers={"cookie": cookie, "origin": origin},
        )
        assert asyncio.run(record(
            env, denied, {"sessionToken": "cookie"})) == ("", None)

    # Safe reads may use the HttpOnly cookie without an Origin header.
    read = _session_request(headers={"cookie": cookie})
    assert asyncio.run(record(
        env, read, {"sessionToken": "cookie"}))[0] == "account-bi"


def test_cookie_marker_uses_cookie_and_mutations_require_exact_origin():
    namespace = {
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "_cookie_value": lambda request, _name: request.cookie,
        "ACCOUNT_SESSION_COOKIE": "forkmesh_account",
        "urlparse": urlparse,
    }
    exec(_source("_request_account_session_token"), with_maintenance_off(namespace))
    exec(_source("_request_same_origin"), with_maintenance_off(namespace))
    request = SimpleNamespace(
        url="https://forkmesh.com/api/profile",
        cookie="v2." + "a" * 32 + "." + "b" * 43,
        headers={
            "authorization": "Bearer cookie",
            "origin": "https://forkmesh.com",
        },
    )
    token, cookie_auth = namespace["_request_account_session_token"](
        request, {"sessionToken": "cookie"})
    assert token == request.cookie
    assert cookie_auth is True
    assert namespace["_request_same_origin"](request) is True
    request.headers["origin"] = "https://mirror.forkmesh.com"
    assert namespace["_request_same_origin"](request) is False


def test_logout_reset_delete_and_rotation_revoke_sessions():
    logout = _source("_account_logout")
    reset = _source("_account_reset_password")
    rotate = _source("_account_rotate")
    delete = _source("_delete_account_namespace")
    rename = _source("_rename_account_namespace")
    assert "_account_revoke_sessions(env, account_bi, session_id)" in logout
    assert "_clear_account_session_cookie()" in logout
    assert "_account_revoke_sessions(env, name_bi)" in reset
    assert "_account_revoke_sessions(env, name_bi)" in rotate
    assert "DELETE FROM account_sessions WHERE account_bi=?" in delete
    assert "UPDATE account_sessions SET account_bi=?" in rename


def test_browser_storage_contains_only_cookie_marker_on_https():
    public = ROOT / "public"
    surfaces = (
        public / "login.js",
        public / "signup.js",
        public / "dashboard.js",
        public / "dashboard" / "js" / "02-helpers.js",
        public / "dashboard-chat.js",
        public / "chat.js",
        ROOT.parent / "world" / "public" / "world" / "world.js",
    )
    for surface in surfaces:
        text = surface.read_text(encoding="utf-8")
        assert '"cookie"' in text, surface
        assert 'location.protocol === "https:"' in text, surface


def test_session_migration_applies_to_sqlite():
    migration = (
        ROOT / "migrations" / "0073_revocable_account_sessions.sql"
    ).read_text(encoding="utf-8")
    database = sqlite3.connect(":memory:")
    database.executescript(migration)
    columns = {
        row[1]
        for row in database.execute("PRAGMA table_info(account_sessions)")
    }
    assert {
        "session_id",
        "account_bi",
        "token_digest",
        "expires_at",
        "revoked_at",
    }.issubset(columns)
    database.close()


def test_token_digest_is_keyed_and_domain_separated():
    namespace = {
        "hmac": hmac,
        "_account_session_secret": lambda _env: b"server-secret",
    }
    exec(_source("_account_session_token_digest"), with_maintenance_off(namespace))
    digest = namespace["_account_session_token_digest"](object(), "bearer")
    assert digest == hmac.new(
        b"server-secret",
        b"forkmesh-account-session-token-v2\nbearer",
        hashlib.sha256,
    ).hexdigest()

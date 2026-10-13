"""Owner-visible, privacy-preserving remote account-session management."""

import ast
import asyncio
import json
from pathlib import Path
import re
from types import SimpleNamespace


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


def _runtime(
    rows=None,
    owned=True,
    now=2_000_000,
    *,
    cookie_auth=False,
    same_origin=True,
    valid_session=True,
    account_rec=None,
    activity_seen=0,
):
    calls = []
    audits = []
    lookups = []

    async def all_rows(_env, sql, *args):
        calls.append(("all", sql, args))
        return list(rows or [])

    async def first(_env, sql, *args):
        calls.append(("first", sql, args))
        if "MAX(seen)" in sql:
            return {"seen": activity_seen}
        return {"session_id": args[1]} if owned else None

    async def run(_env, sql, *args):
        calls.append(("run", sql, args))

    async def lookup(_env, token, touch=True):
        lookups.append((token, touch))
        if not valid_session or not token:
            return "", None, ""
        return ("account-bi", dict(account_rec or {"name": "alice"}),
                "c" * 32)

    async def revoke(_env, account_bi, session_id=""):
        calls.append(("revoke", account_bi, session_id))

    async def invalid_error(_env, _token):
        return "invalid_session"

    async def audit(_env, *args):
        audits.append(args)

    async def append_portable(
            _env, _request, response, _token, _rec, reuse_current=False):
        # Listing reads must not re-sign a grant the browser already holds.
        assert reuse_current is True
        return response

    def response(body, status=200, **kwargs):
        return {"body": body, "status": status, **kwargs}

    namespace = {
        "ACCOUNT_EMAIL_STATUS_DELIVERED": "delivered",
        "ACCOUNT_EMAIL_STATUS_FAILED": "failed",
        "ACCOUNT_SESSION_MAX_ACTIVE": 20,
        "Date": SimpleNamespace(now=lambda: now),
        "MAX_NODE_NAME": 63,
        "_account_revoke_sessions": revoke,
        "_account_session_lookup": lookup,
        "_account_session_invalid_error": invalid_error,
        "_audit_sensitive_action": audit,
        "_append_portable_session_cookie": append_portable,
        "_clear_all_account_cookies": (
            lambda _env, _request, response: response),
        "_clear_account_session_cookie": lambda: "cleared",
        "_account_session_cookie": lambda token: "renewed:" + token,
        "_request_account_session_token": lambda _request, _data: (
            "cookie-token" if cookie_auth else "bearer", cookie_auth),
        "_request_same_origin": lambda _request: same_origin,
        "_request_origin_allowed": lambda _env, _request: same_origin,
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "d1_all": all_rows,
        "d1_first": first,
        "d1_run": run,
        "json_response": response,
        "method_name": lambda request: request.method,
        "re": re,
    }
    exec(_source("_account_last_seen"), namespace)
    exec(_source("_account_email_activity"), namespace)
    exec(_source("_account_sessions"), namespace)
    return namespace["_account_sessions"], calls, audits, lookups


def test_session_list_is_account_scoped_and_shows_only_owner_visible_detail():
    sessions, calls, _audits, lookups = _runtime(rows=[{
        "session_id": "c" * 32,
        "created_at": 1_000_000,
        "last_seen_at": 1_500_000,
        "expires_at": 3_000_000,
        "device_label": "Mobile browser",
        "client_ip": "203.0.113.9",
        "user_agent": "sensitive raw agent",
    }])
    response = asyncio.run(sessions(
        object(), SimpleNamespace(method="GET", headers={})))
    assert response["status"] == 200
    # The sign-in address is returned so the owner can recognize a device they
    # do not own before revoking it. The raw user agent still never leaves D1.
    assert response["body"]["sessions"] == [{
        "id": "c" * 32,
        "deviceLabel": "Mobile browser",
        "ipAddress": "203.0.113.9",
        "createdAt": 1_000_000,
        "lastSeenAt": 1_500_000,
        "expiresAt": 3_000_000,
        "current": True,
    }]
    # Account-level activity for the settings screen: last seen anywhere and
    # the last email that went out, with its outcome.
    assert response["body"]["account"] == {
        "nodeName": "alice",
        "status": "active",
        "emailVerified": False,
        "kind": "user",
        "lastSeenAt": 1_500_000,
        "lastEmailAt": 0,
        "lastEmailStatus": "",
        "lastEmailKind": "",
        "emailSendCount": 0,
    }
    encoded = json.dumps(response)
    assert "sensitive raw agent" not in encoded
    query = next(call for call in calls if call[0] == "all")
    assert "WHERE account_bi=?" in query[1]
    assert query[2][0] == "account-bi"
    assert lookups == [("bearer", True)]


def test_session_list_reports_last_seen_and_last_email():
    rows = [{
        "session_id": "c" * 32,
        "created_at": 1_000_000,
        "last_seen_at": 1_500_000,
        "expires_at": 3_000_000,
        "device_label": "Mobile browser",
    }]
    sessions, calls, _audits, _lookups = _runtime(
        rows=rows,
        # A linked device was seen more recently than any browser session.
        activity_seen=1_900_000,
        account_rec={
            "name": "alice",
            "last_email_ts": 1_700_000,
            "last_email_kind": "notifications",
            "last_email_ok": False,
            "email_send_count": 4,
        })
    response = asyncio.run(sessions(
        object(), SimpleNamespace(method="GET", headers={})))
    assert response["body"]["account"] == {
        "nodeName": "alice",
        "status": "active",
        "emailVerified": False,
        "kind": "user",
        "lastSeenAt": 1_900_000,
        "lastEmailAt": 1_700_000,
        "lastEmailStatus": "failed",
        "lastEmailKind": "notifications",
        # How many account-directed emails have gone out in total.
        "emailSendCount": 4,
    }
    activity = next(
        call for call in calls if call[0] == "first" and "MAX(seen)" in call[1])
    assert activity[2] == ("account-bi", "account-bi")
    # No subject, body, or recipient address is stored or returned.
    assert "@" not in json.dumps(response["body"]["account"])


def test_cookie_session_list_renews_the_long_lived_cookie():
    # Browsers clamp cookie lifetimes (Chromium: 400 days), so the read every
    # signed-in surface makes on load re-issues the multi-year account cookie.
    sessions, _calls, _audits, _lookups = _runtime(cookie_auth=True)
    response = asyncio.run(sessions(
        object(), SimpleNamespace(method="GET", headers={})))
    assert response["status"] == 200
    assert response["extra_headers"] == {
        "Set-Cookie": "renewed:cookie-token"}

    # A bearer caller (desktop, mobile, extension) never gets a browser cookie.
    sessions, _calls, _audits, _lookups = _runtime(cookie_auth=False)
    response = asyncio.run(sessions(
        object(), SimpleNamespace(method="GET", headers={})))
    assert response["extra_headers"] is None


def test_session_revoke_is_scoped_and_audited():
    sessions, calls, audits, lookups = _runtime()
    target = "d" * 32
    response = asyncio.run(sessions(
        object(), SimpleNamespace(method="DELETE", headers={}), target))
    assert response["status"] == 200
    ownership = next(call for call in calls if call[0] == "first")
    assert "account_bi=? AND session_id=?" in ownership[1]
    assert ownership[2][:2] == ("account-bi", target)
    assert ("revoke", "account-bi", target) in calls
    assert audits and audits[0][1:6] == (
        "account.session_revoke",
        "account_session",
        target,
        "success",
        {"scope": "device"},
    )
    assert lookups == [("bearer", False)]


def test_cross_account_or_unknown_session_cannot_be_revoked():
    sessions, calls, audits, _lookups = _runtime(owned=False)
    response = asyncio.run(sessions(
        object(), SimpleNamespace(method="DELETE", headers={}), "d" * 32))
    assert response["status"] == 404
    assert not any(call[0] == "revoke" for call in calls)
    assert audits == []


def test_all_other_sessions_preserves_current_session():
    sessions, calls, audits, _lookups = _runtime()
    response = asyncio.run(sessions(
        object(), SimpleNamespace(method="DELETE", headers={}), "others"))
    assert response["status"] == 200
    update = next(call for call in calls if call[0] == "run")
    assert "account_bi=? AND session_id<>?" in update[1]
    assert update[2] == (2_000_000, "account-bi", "c" * 32)
    assert audits[0][-1] == {"scope": "others"}


def test_log_out_everywhere_drops_the_calling_device_too():
    sessions, calls, audits, _lookups = _runtime()
    response = asyncio.run(sessions(
        object(), SimpleNamespace(method="DELETE", headers={}), "all"))
    assert response["status"] == 200
    assert response["body"]["currentRevoked"] is True
    assert response["extra_headers"]["Set-Cookie"] == "cleared"
    # An account-wide revoke, not a per-session one: no session id is passed.
    assert ("revoke", "account-bi", "") in calls
    assert audits[0][1:6] == (
        "account.session_revoke",
        "account_session",
        "all-devices",
        "success",
        {"scope": "all"},
    )


def test_cookie_delete_requires_exact_origin():
    sessions, calls, audits, lookups = _runtime(
        cookie_auth=True, same_origin=False)
    response = asyncio.run(sessions(
        object(), SimpleNamespace(method="DELETE", headers={}), "d" * 32))
    assert response["status"] == 401
    assert lookups == [("", False)]
    assert not any(call[0] in ("run", "revoke") for call in calls)
    assert audits == []


def test_current_session_revoke_clears_cookie():
    sessions, calls, audits, _lookups = _runtime(cookie_auth=True)
    response = asyncio.run(sessions(
        object(), SimpleNamespace(method="DELETE", headers={}), "c" * 32))
    assert response["status"] == 200
    assert response["body"]["currentRevoked"] is True
    assert response["extra_headers"]["Set-Cookie"] == "cleared"
    assert ("revoke", "account-bi", "c" * 32) in calls
    assert audits[0][-1] == {"scope": "current"}


def test_invalid_session_cannot_list_or_revoke():
    sessions, calls, audits, _lookups = _runtime(valid_session=False)
    for method, target in (("GET", ""), ("DELETE", "d" * 32)):
        response = asyncio.run(sessions(
            object(), SimpleNamespace(method=method, headers={}), target))
        assert response["status"] == 401
    assert not any(call[0] in ("all", "run", "revoke") for call in calls)
    assert audits == []


def test_frontend_exposes_remote_session_controls_and_privacy_copy():
    fragment = (
        ROOT / "public" / "dashboard" / "js" / "04-account.js"
    ).read_text(encoding="utf-8")
    settings = (
        ROOT / "public" / "dashboard" / "partials" / "views" / "settings.html"
    ).read_text(encoding="utf-8")
    assert 'fetch("/api/accounts/sessions" + path' in fragment
    assert 'accountSessionApi("DELETE", "/others")' in fragment
    assert "data-account-session-revoke" in fragment
    assert "data-account-session-list" in settings
    assert "the address the sign-in came from" in settings
    assert "session.ipAddress" in fragment


def test_session_routes_are_bounded_to_get_and_delete():
    handler = _source("accounts_handler")
    assert (
        'url.path.rstrip("/") == "/api/accounts/sessions" '
        'and method == "GET"'
    ) in handler
    assert 'account_session_prefix = "/api/accounts/sessions/"' in handler
    assert 'and method == "DELETE"' in handler

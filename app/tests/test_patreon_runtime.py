"""Behavioral Patreon OAuth, webhook, and lifecycle tests."""

import asyncio
import base64
import hashlib
import hmac
import json
import sqlite3
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import pytest


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
sys.path.insert(0, str(SRC))

import patreon_handler as handler  # noqa: E402


NOW = 2_000_000_000_000
FUTURE = "2033-05-18T04:33:20Z"
REAL_PROVIDER_JSON = handler._provider_json
MEMBER_URL = "https://www.patreon.com/api/oauth2/v2/members/member-1"


class FakeDate:
    now_value = NOW

    @classmethod
    def now(cls):
        return cls.now_value


class FakeResponse:
    def __init__(self, body="", status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = dict(headers or {})


class ChunkReader:
    def __init__(self, chunks):
        self.chunks = iter(chunks)
        self.cancelled = False

    async def read(self):
        try:
            return {"done": False, "value": next(self.chunks)}
        except StopIteration:
            return {"done": True, "value": None}

    async def cancel(self):
        self.cancelled = True


class ChunkBody:
    def __init__(self, chunks):
        self.reader = ChunkReader(chunks)

    def getReader(self):
        return self.reader


class ChunkedProviderResponse:
    def __init__(self, chunks):
        self.status = 200
        self.headers = {}
        self.body = ChunkBody(chunks)

    async def text(self):
        raise AssertionError("streaming responses must not call text()")


class FakeRequest:
    def __init__(
        self, url, method="GET", body="", headers=None, authenticated=True,
        same_origin=True, account_bi="acct-a", session_id="session-1",
    ):
        self.url = url
        self.method = method
        self._body = body
        self.headers = dict(headers or {})
        self.authenticated = authenticated
        self.same_origin = same_origin
        self.account_bi = account_bi
        self.session_id = session_id

    async def text(self):
        return self._body


class Runtime:
    def __init__(self):
        self.connection = sqlite3.connect(":memory:", isolation_level=None)
        self.connection.row_factory = sqlite3.Row
        self.fail_batch_on = ""
        self.before_batch = None
        self.provider = {}
        self.provider_calls = []
        self.PATREON_CLIENT_ID = "client-id"
        self.PATREON_CLIENT_SECRET = "client-secret"
        self.PATREON_CAMPAIGN_ID = "campaign-1"
        self.PATREON_CREATOR_ACCESS_TOKEN = "creator-access-token"  # forkmesh-secret-scan:ignore-line
        self.PATREON_OAUTH_REDIRECT_URI = (
            "https://forkmesh.test/api/integrations/patreon/callback"
        )
        self.PATREON_WEBHOOK_SECRET = "webhook-secret"
        self.connection.executescript(
            """
            CREATE TABLE users (
                user_bi TEXT PRIMARY KEY,
                data TEXT NOT NULL,
                email_bi TEXT
            );
            CREATE TABLE account_sessions (
                session_id TEXT PRIMARY KEY,
                account_bi TEXT NOT NULL,
                expires_at INTEGER NOT NULL,
                revoked_at INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE patreon_oauth_states (
                state_hash TEXT PRIMARY KEY,
                account_bi TEXT NOT NULL,
                data TEXT NOT NULL,
                created_at INTEGER NOT NULL,
                expires_at INTEGER NOT NULL
            );
            CREATE TABLE patreon_account_links (
                account_bi TEXT PRIMARY KEY,
                patreon_user_bi TEXT NOT NULL UNIQUE,
                data TEXT NOT NULL,
                linked_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL
            );
            CREATE TABLE role_grants (
                account_bi TEXT NOT NULL,
                role TEXT NOT NULL,
                scope_type TEXT NOT NULL,
                scope_bi TEXT NOT NULL,
                granted_by_bi TEXT,
                granted_at INTEGER NOT NULL,
                expires_at INTEGER NOT NULL,
                revoked_at INTEGER NOT NULL,
                PRIMARY KEY (account_bi, role, scope_type, scope_bi)
            );
            CREATE TABLE patreon_webhook_events (
                event_id TEXT PRIMARY KEY,
                received_at INTEGER NOT NULL,
                event_type TEXT NOT NULL,
                account_bi TEXT NOT NULL
            );
            CREATE TABLE patreon_reconciliation_generations (
                member_bi TEXT PRIMARY KEY,
                generation INTEGER NOT NULL CHECK (generation >= 1),
                updated_at INTEGER NOT NULL CHECK (updated_at >= 0)
            );
            CREATE TABLE sensitive_audit_log (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts INTEGER NOT NULL,
                actor_bi TEXT,
                actor_label TEXT NOT NULL,
                action TEXT NOT NULL,
                target_type TEXT NOT NULL,
                target_token TEXT NOT NULL,
                outcome TEXT NOT NULL,
                details TEXT NOT NULL
            );
            """
        )


def run(coro):
    return asyncio.run(coro)


def encode(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def decode(value):
    return json.loads(value)


def sealed(value):
    payload = base64.urlsafe_b64encode(encode(value).encode()).decode()
    return "sealed:" + payload


def unseal(value):
    if str(value).startswith("sealed:"):
        raw = base64.urlsafe_b64decode(str(value)[7:]).decode()
        return decode(raw)
    return decode(value)


def bi(value):
    return hashlib.sha256(str(value).encode()).hexdigest()


@pytest.fixture
def runtime(monkeypatch):
    FakeDate.now_value = NOW
    env = Runtime()

    async def d1_first(current_env, sql, *args):
        row = current_env.connection.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(current_env, sql, *args):
        current_env.connection.execute(sql, args)

    async def d1_batch(current_env, statements):
        if current_env.before_batch:
            callback = current_env.before_batch
            current_env.before_batch = None
            callback()
        current_env.connection.execute("BEGIN")
        try:
            for sql, args in statements:
                current_env.connection.execute(sql, args)
                if (
                    current_env.fail_batch_on
                    and current_env.fail_batch_on in sql
                ):
                    raise RuntimeError("injected batch failure")
        except Exception:
            current_env.connection.execute("ROLLBACK")
            raise
        current_env.connection.execute("COMMIT")

    async def encrypt_row(_env, value):
        return sealed(value)

    async def decrypt_row(_env, value):
        try:
            return unseal(value)
        except Exception:
            return None

    async def blind_index(_env, value):
        return bi(value)

    async def account_session_record(current_env, request, _data):
        if not request.authenticated:
            return "", None
        account_bi = getattr(request, "account_bi", "acct-a")
        row = current_env.connection.execute(
            "SELECT data FROM users WHERE user_bi=?", (account_bi,)
        ).fetchone()
        return (account_bi, unseal(row[0])) if row else ("", None)

    async def provider_json(url, method="GET", form=None, access_token=""):
        env.provider_calls.append({
            "url": url,
            "method": method,
            "form": form,
            "access_token": access_token,
        })
        result = env.provider.get((url, method))
        if isinstance(result, Exception):
            raise result
        if callable(result):
            result = result()
        if hasattr(result, "__await__"):
            result = await result
        return result or (500, None)

    def response(data, status=200, cache_control=None, extra_headers=None):
        headers = dict(extra_headers or {})
        if cache_control:
            headers["cache-control"] = cache_control
        return FakeResponse(encode(data), status, headers)

    monkeypatch.setitem(handler.__dict__, "_bind_entry_symbols", lambda: None)
    monkeypatch.setitem(handler.__dict__, "Date", FakeDate)
    monkeypatch.setitem(handler.__dict__, "Response", FakeResponse)
    monkeypatch.setitem(handler.__dict__, "_random_bytes", lambda size: b"x" * size)
    monkeypatch.setitem(
        handler.__dict__, "_request_same_origin",
        lambda request: request.same_origin,
    )
    monkeypatch.setitem(
        handler.__dict__, "_request_account_session_id",
        lambda request, _data: request.session_id if request.authenticated else "",
    )
    monkeypatch.setitem(
        handler.__dict__, "method_name", lambda request: request.method
    )
    monkeypatch.setitem(handler.__dict__, "json_response", response)
    monkeypatch.setitem(handler.__dict__, "ensure_schema", _noop)
    monkeypatch.setitem(handler.__dict__, "d1_first", d1_first)
    monkeypatch.setitem(handler.__dict__, "d1_run", d1_run)
    monkeypatch.setitem(
        handler.__dict__, "_contribution_run_batch", d1_batch
    )
    monkeypatch.setitem(handler.__dict__, "encrypt_row", encrypt_row)
    monkeypatch.setitem(handler.__dict__, "decrypt_row", decrypt_row)
    monkeypatch.setitem(handler.__dict__, "blind_index", blind_index)
    monkeypatch.setitem(
        handler.__dict__, "_account_session_record", account_session_record
    )
    monkeypatch.setattr(handler, "_provider_json", provider_json)
    monkeypatch.setitem(handler.__dict__, "_audit_sensitive_action", _noop)
    return env


async def _noop(*_args, **_kwargs):
    return None


def add_account(env, account_bi, email="", session_id="session-1",
                email_verified=True):
    env.connection.execute(
        "INSERT INTO users(user_bi,data,email_bi) VALUES (?,?,?)",
        (
            account_bi,
            encode({"name": account_bi, "status": "active", "kind": "user",
                    "email_verified": email_verified}),
            bi(email) if email else None,
        ),
    )
    env.connection.execute(
        "INSERT INTO account_sessions(session_id,account_bi,expires_at,revoked_at) "
        "VALUES (?,?,?,0)",
        (session_id, account_bi, NOW + 60_000),
    )


def add_state(env, account_bi="acct-a", session_id="session-1", expires=None):
    state = "s" * 43
    record = {
        "state": state,
        "accountBi": account_bi,
        "sessionId": session_id,
        "redirectUri": env.PATREON_OAUTH_REDIRECT_URI,
        "createdAt": NOW,
    }
    env.connection.execute(
        "INSERT INTO patreon_oauth_states "
        "(state_hash,account_bi,data,created_at,expires_at) VALUES (?,?,?,?,?)",
        (
            handler._state_hash(state),
            account_bi,
            encode(record),
            NOW,
            NOW + 60_000 if expires is None else expires,
        ),
    )
    return state


def identity(
    amount=1000, next_charge=FUTURE, user_id="patreon-a",
    campaign_id="campaign-1",
):
    return {
        "data": {
            "type": "user",
            "id": user_id,
            "relationships": {
                "memberships": {
                    "data": [{"type": "member", "id": "member-1"}]
                }
            },
        },
        "included": [{
            "type": "member",
            "id": "member-1",
            "attributes": {
                "currently_entitled_amount_cents": amount,
                "patron_status": "active_patron",
                "next_charge_date": next_charge,
            },
            "relationships": {
                "campaign": {
                    "data": {"type": "campaign", "id": campaign_id}
                }
            },
        }],
    }


def member_snapshot(
    amount=1000,
    next_charge=FUTURE,
    user_id="patreon-a",
    email="a@example.test",
    campaign_id="campaign-1",
    patron_status="active_patron",
):
    return {
        "data": {
            "type": "member",
            "id": "member-1",
            "attributes": {
                "currently_entitled_amount_cents": amount,
                "email": email,
                "patron_status": patron_status,
                "next_charge_date": next_charge,
            },
            "relationships": {
                "campaign": {
                    "data": {"type": "campaign", "id": campaign_id}
                },
                "user": {"data": {"type": "user", "id": user_id}},
            },
        },
        "included": [{
            "type": "user",
            "id": user_id,
            "attributes": {},
        }],
    }


def callback_request(state):
    return FakeRequest(
        "https://forkmesh.test/api/integrations/patreon/callback?"
        + "state="
        + state
        + "&code=provider-code"
    )


def callback_outcome(response):
    return parse_qs(urlparse(response.headers["location"]).query)["patreon"][0]


def configure_provider(env, payload=None):
    env.provider[(handler.PATREON_TOKEN_URL, "POST")] = (
        200,
        {"access_token": "temporary-token"},
    )
    env.provider[(handler.PATREON_IDENTITY_URL, "GET")] = (
        200,
        payload or identity(),
    )


def active_roles(env, account_bi):
    return {
        row["role"]
        for row in env.connection.execute(
            "SELECT role FROM role_grants WHERE account_bi=? AND revoked_at=0",
            (account_bi,),
        )
    }


def test_oauth_uses_redirect_uri_configuration(runtime):
    assert handler._oauth_config(runtime)["redirectUri"] == (
        runtime.PATREON_OAUTH_REDIRECT_URI
    )


def test_oauth_start_requires_authentication_and_same_origin(runtime):
    add_account(runtime, "acct-a")
    unauthenticated = FakeRequest(
        "https://forkmesh.test/api/accounts/patreon/oauth/start",
        method="POST",
        authenticated=False,
    )
    cross_origin = FakeRequest(
        "https://forkmesh.test/api/accounts/patreon/oauth/start",
        method="POST",
        same_origin=False,
    )

    unauthenticated_response = run(
        handler.patreon_oauth_start_handler(runtime, unauthenticated)
    )
    cross_origin_response = run(
        handler.patreon_oauth_start_handler(runtime, cross_origin)
    )

    assert unauthenticated_response.status == 401
    assert decode(unauthenticated_response.body)["error"] == "invalid_session"
    assert cross_origin_response.status == 403
    assert decode(cross_origin_response.body)["error"] == "origin_not_allowed"
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_oauth_states"
    ).fetchone()[0] == 0


def test_oauth_start_persists_encrypted_ten_minute_single_use_state(runtime):
    add_account(runtime, "acct-a")
    request = FakeRequest(
        "https://forkmesh.test/api/accounts/patreon/oauth/start",
        method="POST",
    )

    response = run(handler.patreon_oauth_start_handler(runtime, request))

    payload = decode(response.body)
    state = parse_qs(
        urlparse(payload["authorizationUrl"]).query
    )["state"][0]
    row = runtime.connection.execute(
        "SELECT * FROM patreon_oauth_states"
    ).fetchone()
    assert response.status == 200
    assert row["state_hash"] == handler._state_hash(state)
    assert row["expires_at"] - row["created_at"] == 10 * 60 * 1000
    assert row["data"].startswith("sealed:")
    assert state not in row["data"]
    record = unseal(row["data"])
    assert record["accountBi"] == "acct-a"
    assert record["sessionId"] == "session-1"


@pytest.mark.parametrize(
    ("setup", "expected"),
    [
        ("expired", "expired"),
        ("replay", "replayed"),
        ("session_mismatch", "mismatch"),
        ("provider_failure", "failed"),
        ("provider_exception", "failed"),
        ("duplicate_identity", "duplicate_identity"),
    ],
)
def test_callback_failure_paths_are_behavioral_safe_redirects(
    runtime, setup, expected
):
    add_account(runtime, "acct-a")
    state = add_state(
        runtime, expires=NOW - 1 if setup == "expired" else None
    )
    configure_provider(runtime)
    if setup == "replay":
        runtime.connection.execute("DELETE FROM patreon_oauth_states")
    elif setup == "session_mismatch":
        runtime.connection.execute(
            "UPDATE account_sessions SET revoked_at=1"
        )
    elif setup == "provider_failure":
        runtime.provider[(handler.PATREON_TOKEN_URL, "POST")] = (
            503,
            None,
        )
    elif setup == "provider_exception":
        runtime.provider[(handler.PATREON_TOKEN_URL, "POST")] = RuntimeError(
            "provider unavailable"
        )
    elif setup == "duplicate_identity":
        add_account(runtime, "acct-b", session_id="session-2")
        runtime.connection.execute(
            "INSERT INTO patreon_account_links "
            "(account_bi,patreon_user_bi,data,linked_at,updated_at) "
            "VALUES (?,?,?,?,?)",
            ("acct-b", bi("patreon-a"), encode({"tier": "pro"}), NOW, NOW),
        )

    response = run(
        handler.patreon_oauth_callback_handler(runtime, callback_request(state))
    )

    assert response.status == 303
    assert callback_outcome(response) == expected
    assert response.headers["location"] == (
        "/dashboard/settings/account?patreon=" + expected
    )
    assert active_roles(runtime, "acct-a") == set()


def test_callback_rejects_encrypted_state_account_mismatch(runtime):
    add_account(runtime, "acct-a")
    state = add_state(runtime)
    row = runtime.connection.execute(
        "SELECT data FROM patreon_oauth_states"
    ).fetchone()
    record = unseal(row["data"])
    record["accountBi"] = "acct-other"
    runtime.connection.execute(
        "UPDATE patreon_oauth_states SET data=?",
        (sealed(record),),
    )

    response = run(
        handler.patreon_oauth_callback_handler(runtime, callback_request(state))
    )

    assert response.status == 303
    assert callback_outcome(response) == "mismatch"
    assert active_roles(runtime, "acct-a") == set()


def test_callback_wrong_campaign_has_fixed_safe_outcome(runtime):
    add_account(runtime, "acct-a")
    state = add_state(runtime)
    configure_provider(runtime, identity(campaign_id="campaign-other"))

    response = run(
        handler.patreon_oauth_callback_handler(runtime, callback_request(state))
    )

    assert response.status == 303
    assert callback_outcome(response) == "wrong_campaign"
    assert response.headers["location"] == (
        "/dashboard/settings/account?patreon=wrong_campaign"
    )
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_account_links"
    ).fetchone()[0] == 0
    assert active_roles(runtime, "acct-a") == set()


def test_callback_rejects_positive_membership_without_valid_expiry(runtime):
    add_account(runtime, "acct-a")
    state = add_state(runtime)
    configure_provider(runtime, identity(next_charge="not-a-date"))

    response = run(
        handler.patreon_oauth_callback_handler(runtime, callback_request(state))
    )

    assert callback_outcome(response) == "invalid_membership"
    assert active_roles(runtime, "acct-a") == set()
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_account_links"
    ).fetchone()[0] == 0


def test_successful_callback_state_cannot_be_replayed(runtime):
    add_account(runtime, "acct-a")
    state = add_state(runtime)
    configure_provider(runtime)

    first = run(
        handler.patreon_oauth_callback_handler(runtime, callback_request(state))
    )
    replay = run(
        handler.patreon_oauth_callback_handler(runtime, callback_request(state))
    )

    assert callback_outcome(first) == "connected"
    assert callback_outcome(replay) == "replayed"
    assert active_roles(runtime, "acct-a") == {
        "pro_member",
        "supporting_member",
    }
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_account_links"
    ).fetchone()[0] == 1


@pytest.mark.parametrize(
    ("amount", "tier", "roles"),
    (
        (
            5000,
            "builder",
            {
                "builder_member", "pro_member", "supporting_member",
            },
        ),
        (
            10000,
            "studio",
            {
                "studio_member", "builder_member", "pro_member",
                "supporting_member",
            },
        ),
    ),
)
def test_higher_campaign_tiers_inherit_pro_access(runtime, amount, tier, roles):
    add_account(runtime, "acct-a")
    state = add_state(runtime)
    configure_provider(runtime, identity(amount=amount))

    response = run(
        handler.patreon_oauth_callback_handler(runtime, callback_request(state))
    )
    projection = unseal(runtime.connection.execute(
        "SELECT data FROM patreon_account_links WHERE account_bi='acct-a'"
    ).fetchone()[0])
    account = unseal(runtime.connection.execute(
        "SELECT data FROM users WHERE user_bi='acct-a'"
    ).fetchone()[0])

    assert callback_outcome(response) == "connected"
    assert projection["tier"] == tier
    assert account["account_tier"] == "pro"
    assert account["patreon_tier"] == tier
    assert active_roles(runtime, "acct-a") == roles


def test_callback_batch_failure_rolls_back_link_roles_and_user(runtime):
    add_account(runtime, "acct-a")
    before = runtime.connection.execute(
        "SELECT data FROM users WHERE user_bi='acct-a'"
    ).fetchone()[0]
    state = add_state(runtime)
    configure_provider(runtime)
    runtime.fail_batch_on = "UPDATE users SET data="

    response = run(
        handler.patreon_oauth_callback_handler(runtime, callback_request(state))
    )

    assert callback_outcome(response) == "failed"
    assert active_roles(runtime, "acct-a") == set()
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_account_links"
    ).fetchone()[0] == 0
    assert runtime.connection.execute(
        "SELECT data FROM users WHERE user_bi='acct-a'"
    ).fetchone()[0] == before


def test_callback_reloads_account_after_provider_requests(runtime):
    add_account(runtime, "acct-a")
    state = add_state(runtime)
    configure_provider(runtime)

    def update_profile():
        runtime.connection.execute(
            "UPDATE users SET data=? WHERE user_bi='acct-a'",
            (encode({
                "kind": "user",
                "name": "renamed profile",
                "status": "active",
                "timezone": "UTC",
            }),),
        )
        return (200, identity())

    runtime.provider[(handler.PATREON_IDENTITY_URL, "GET")] = update_profile

    response = run(
        handler.patreon_oauth_callback_handler(runtime, callback_request(state))
    )
    account = unseal(runtime.connection.execute(
        "SELECT data FROM users WHERE user_bi='acct-a'"
    ).fetchone()[0])

    assert callback_outcome(response) == "connected"
    assert account["name"] == "renamed profile"
    assert account["timezone"] == "UTC"
    assert account["account_tier"] == "pro"


def test_callback_does_not_restore_account_deleted_during_provider_call(runtime):
    add_account(runtime, "acct-a")
    state = add_state(runtime)
    configure_provider(runtime)

    def delete_account():
        runtime.connection.execute(
            "DELETE FROM account_sessions WHERE account_bi='acct-a'"
        )
        runtime.connection.execute("DELETE FROM users WHERE user_bi='acct-a'")
        return (200, identity())

    runtime.provider[(handler.PATREON_IDENTITY_URL, "GET")] = delete_account

    response = run(
        handler.patreon_oauth_callback_handler(runtime, callback_request(state))
    )

    assert callback_outcome(response) == "mismatch"
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM users WHERE user_bi='acct-a'"
    ).fetchone()[0] == 0
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_account_links"
    ).fetchone()[0] == 0
    assert active_roles(runtime, "acct-a") == set()


def test_callback_conditional_batch_rejects_profile_race(runtime):
    add_account(runtime, "acct-a")
    state = add_state(runtime)
    configure_provider(runtime)

    def update_profile_before_batch():
        runtime.connection.execute(
            "UPDATE users SET data=? WHERE user_bi='acct-a'",
            (encode({
                "kind": "user",
                "name": "concurrent profile",
                "status": "active",
                "locale": "en-GB",
            }),),
        )

    runtime.before_batch = update_profile_before_batch
    response = run(
        handler.patreon_oauth_callback_handler(runtime, callback_request(state))
    )
    account = unseal(runtime.connection.execute(
        "SELECT data FROM users WHERE user_bi='acct-a'"
    ).fetchone()[0])

    assert callback_outcome(response) == "mismatch"
    assert account["name"] == "concurrent profile"
    assert account["locale"] == "en-GB"
    assert "account_tier" not in account
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_account_links"
    ).fetchone()[0] == 0
    assert active_roles(runtime, "acct-a") == set()


def webhook_request(
    env, event, amount, user_id, email, next_charge=FUTURE,
    configure_authoritative=True,
):
    payload = {
        "data": {
            "id": "member-1",
            "type": "member",
            "attributes": {
                "currently_entitled_amount_cents": amount,
                "email": email,
                "patron_status": (
                    "former_patron"
                    if event == "members:pledge:delete"
                    else "active_patron"
                ),
                "next_charge_date": next_charge,
            },
            "relationships": {
                "user": {"data": {"type": "user", "id": user_id}}
            },
        },
    }
    if configure_authoritative:
        env.provider[(MEMBER_URL, "GET")] = (
            200,
            member_snapshot(
                amount=amount,
                next_charge=next_charge,
                user_id=user_id,
                email=email,
                patron_status=(
                    "former_patron"
                    if event == "members:pledge:delete"
                    else "active_patron"
                ),
            ),
        )
    raw = encode(payload)
    signature = hmac.new(
        env.PATREON_WEBHOOK_SECRET.encode(), raw.encode(), hashlib.md5
    ).hexdigest()
    return FakeRequest(
        "https://forkmesh.test/api/integrations/patreon/webhook",
        method="POST",
        body=raw,
        headers={
            "x-patreon-event": event,
            "x-patreon-signature": signature,
        },
    )


def test_webhook_linked_id_wins_conflicting_email(runtime):
    add_account(runtime, "acct-a", "a@example.test")
    add_account(runtime, "acct-b", "b@example.test", "session-2")
    runtime.connection.execute(
        "INSERT INTO patreon_account_links "
        "(account_bi,patreon_user_bi,data,linked_at,updated_at) "
        "VALUES (?,?,?,?,?)",
        ("acct-a", bi("patreon-a"), encode({"tier": "core"}), NOW, NOW),
    )

    response = run(handler.patreon_webhook_handler(
        runtime,
        webhook_request(
            runtime,
            "members:pledge:create",
            1000,
            "patreon-a",
            "b@example.test",
        ),
    ))

    assert response.status == 200
    assert active_roles(runtime, "acct-a") == {
        "pro_member",
        "supporting_member",
    }
    assert active_roles(runtime, "acct-b") == set()


def test_webhook_email_fallback_matches_without_creating_link(runtime):
    add_account(runtime, "acct-a", "a@example.test")

    response = run(handler.patreon_webhook_handler(
        runtime,
        webhook_request(
            runtime,
            "members:pledge:create",
            500,
            "unlinked-patreon-user",
            "a@example.test",
        ),
    ))

    assert response.status == 200
    assert decode(response.body)["matched"] is True
    assert active_roles(runtime, "acct-a") == {"supporting_member"}
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_account_links"
    ).fetchone()[0] == 0


def test_webhook_email_fallback_ignores_an_unconfirmed_email(runtime):
    add_account(runtime, "squatter", "a@example.test", email_verified=False)

    response = run(handler.patreon_webhook_handler(
        runtime,
        webhook_request(
            runtime,
            "members:pledge:create",
            1000,
            "unlinked-patreon-user",
            "a@example.test",
        ),
    ))

    assert response.status == 200
    assert decode(response.body)["matched"] is False
    assert active_roles(runtime, "squatter") == set()


def test_unmatched_webhook_can_be_processed_after_later_link(runtime):
    add_account(runtime, "acct-a", "a@example.test")
    request = webhook_request(
        runtime,
        "members:pledge:create",
        1000,
        "patreon-a",
        "unknown@example.test",
    )

    unmatched = run(handler.patreon_webhook_handler(runtime, request))
    assert decode(unmatched.body)["matched"] is False
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_webhook_events"
    ).fetchone()[0] == 0

    runtime.connection.execute(
        "INSERT INTO patreon_account_links "
        "(account_bi,patreon_user_bi,data,linked_at,updated_at) "
        "VALUES (?,?,?,?,?)",
        ("acct-a", bi("patreon-a"), encode({"tier": "core"}), NOW, NOW),
    )
    matched = run(handler.patreon_webhook_handler(runtime, request))

    assert decode(matched.body)["matched"] is True
    assert active_roles(runtime, "acct-a") == {
        "pro_member",
        "supporting_member",
    }
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_webhook_events"
    ).fetchone()[0] == 1


def test_webhook_replay_is_deduped_without_reapplying(runtime):
    add_account(runtime, "acct-a", "a@example.test")
    runtime.connection.execute(
        "INSERT INTO patreon_account_links "
        "(account_bi,patreon_user_bi,data,linked_at,updated_at) "
        "VALUES (?,?,?,?,?)",
        ("acct-a", bi("patreon-a"), encode({"tier": "core"}), NOW, NOW),
    )
    request = webhook_request(
        runtime,
        "members:pledge:create",
        1000,
        "patreon-a",
        "a@example.test",
    )

    first = run(handler.patreon_webhook_handler(runtime, request))
    replay = run(handler.patreon_webhook_handler(runtime, request))

    assert decode(first.body)["matched"] is True
    assert decode(replay.body) == {"deduped": True, "ok": True}
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_webhook_events"
    ).fetchone()[0] == 1
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM sensitive_audit_log"
    ).fetchone()[0] == 1
    assert active_roles(runtime, "acct-a") == {
        "pro_member",
        "supporting_member",
    }


def test_webhook_replay_remains_deduped_after_membership_expiry(runtime):
    add_account(runtime, "acct-a", "a@example.test")
    runtime.connection.execute(
        "INSERT INTO patreon_account_links "
        "(account_bi,patreon_user_bi,data,linked_at,updated_at) "
        "VALUES (?,?,?,?,?)",
        ("acct-a", bi("patreon-a"), encode({"tier": "core"}), NOW, NOW),
    )
    request = webhook_request(
        runtime,
        "members:pledge:create",
        1000,
        "patreon-a",
        "a@example.test",
    )

    first = run(handler.patreon_webhook_handler(runtime, request))
    FakeDate.now_value = (
        handler.patreon._normalize_expires_at_iso(FUTURE)
        + handler.patreon.SUBSCRIPTION_GRACE_MS
        + 1
    )
    replay = run(handler.patreon_webhook_handler(runtime, request))

    assert decode(first.body)["matched"] is True
    assert replay.status == 200
    assert decode(replay.body) == {"deduped": True, "ok": True}
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_webhook_events"
    ).fetchone()[0] == 1
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM sensitive_audit_log"
    ).fetchone()[0] == 1


def test_webhook_downgrade_and_cancellation_update_link_projection(runtime):
    add_account(runtime, "acct-a", "a@example.test")
    runtime.connection.execute(
        "INSERT INTO patreon_account_links "
        "(account_bi,patreon_user_bi,data,linked_at,updated_at) "
        "VALUES (?,?,?,?,?)",
        ("acct-a", bi("patreon-a"), encode({"tier": "pro"}), NOW, NOW),
    )
    run(handler.patreon_webhook_handler(
        runtime,
        webhook_request(
            runtime,
            "members:pledge:update",
            500,
            "patreon-a",
            "a@example.test",
        ),
    ))
    projection = unseal(runtime.connection.execute(
        "SELECT data FROM patreon_account_links WHERE account_bi='acct-a'"
    ).fetchone()[0])
    assert projection["tier"] == "supporter"
    assert active_roles(runtime, "acct-a") == {"supporting_member"}
    private_response = run(handler.patreon_connection_handler(
        runtime, FakeRequest("https://forkmesh.test/api/accounts/patreon")
    ))
    assert decode(private_response.body)["tier"] == "supporter"

    run(handler.patreon_webhook_handler(
        runtime,
        webhook_request(
            runtime,
            "members:pledge:delete",
            1000,
            "patreon-a",
            "a@example.test",
            next_charge="",
        ),
    ))
    projection = unseal(runtime.connection.execute(
        "SELECT data FROM patreon_account_links WHERE account_bi='acct-a'"
    ).fetchone()[0])
    assert projection["tier"] == "free"
    assert projection["patronStatus"] == "former_patron"
    assert active_roles(runtime, "acct-a") == set()
    private_response = run(handler.patreon_connection_handler(
        runtime, FakeRequest("https://forkmesh.test/api/accounts/patreon")
    ))
    assert decode(private_response.body)["tier"] == "free"


def test_connection_reports_oauth_capability_before_the_ui_enables_linking(runtime):
    add_account(runtime, "acct-a")
    request = FakeRequest("https://forkmesh.test/api/accounts/patreon")

    configured = run(handler.patreon_connection_handler(runtime, request))
    assert decode(configured.body)["configured"] is True

    runtime.PATREON_CLIENT_SECRET = ""
    unavailable = run(handler.patreon_connection_handler(runtime, request))
    assert unavailable.status == 200
    assert decode(unavailable.body)["configured"] is False


def test_stale_update_notification_applies_current_cancelled_snapshot(runtime):
    add_account(runtime, "acct-a", "a@example.test")
    runtime.connection.execute(
        "INSERT INTO patreon_account_links "
        "(account_bi,patreon_user_bi,data,linked_at,updated_at) "
        "VALUES (?,?,?,?,?)",
        ("acct-a", bi("patreon-a"), encode({"tier": "pro"}), NOW, NOW),
    )
    runtime.connection.execute(
        "INSERT INTO role_grants VALUES (?,?,?,?,?,?,?,?)",
        (
            "acct-a", "pro_member", "platform", "", "acct-a",
            NOW, NOW + 60_000, 0,
        ),
    )
    notification = webhook_request(
        runtime,
        "members:pledge:update",
        1000,
        "patreon-a",
        "a@example.test",
        configure_authoritative=False,
    )
    runtime.provider[(MEMBER_URL, "GET")] = (
        200,
        member_snapshot(
            amount=0,
            next_charge="",
            patron_status="former_patron",
        ),
    )

    response = run(handler.patreon_webhook_handler(runtime, notification))
    projection = unseal(runtime.connection.execute(
        "SELECT data FROM patreon_account_links WHERE account_bi='acct-a'"
    ).fetchone()[0])

    assert decode(response.body)["matched"] is True
    assert projection["tier"] == "free"
    assert active_roles(runtime, "acct-a") == set()


def test_stale_delete_notification_applies_current_active_snapshot(runtime):
    add_account(runtime, "acct-a", "a@example.test")
    runtime.connection.execute(
        "INSERT INTO patreon_account_links "
        "(account_bi,patreon_user_bi,data,linked_at,updated_at) "
        "VALUES (?,?,?,?,?)",
        ("acct-a", bi("patreon-a"), encode({"tier": "free"}), NOW, NOW),
    )
    notification = webhook_request(
        runtime,
        "members:pledge:delete",
        0,
        "patreon-a",
        "a@example.test",
        next_charge="",
        configure_authoritative=False,
    )
    runtime.provider[(MEMBER_URL, "GET")] = (
        200,
        member_snapshot(amount=1000),
    )

    response = run(handler.patreon_webhook_handler(runtime, notification))

    assert decode(response.body)["tier"] == "pro"
    assert active_roles(runtime, "acct-a") == {
        "pro_member",
        "supporting_member",
    }


def test_concurrent_older_snapshot_cannot_overwrite_newer_reconciliation(runtime):
    add_account(runtime, "acct-a", "a@example.test")
    runtime.connection.execute(
        "INSERT INTO patreon_account_links "
        "(account_bi,patreon_user_bi,data,linked_at,updated_at) "
        "VALUES (?,?,?,?,?)",
        ("acct-a", bi("patreon-a"), encode({"tier": "pro"}), NOW, NOW),
    )
    for role in ("pro_member", "supporting_member"):
        runtime.connection.execute(
            "INSERT INTO role_grants VALUES (?,?,?,?,?,?,?,?)",
            (
                "acct-a", role, "platform", "", "acct-a",
                NOW, NOW + 60_000, 0,
            ),
        )
    active_notification = webhook_request(
        runtime,
        "members:pledge:update",
        1000,
        "patreon-a",
        "a@example.test",
        configure_authoritative=False,
    )
    cancellation_notification = webhook_request(
        runtime,
        "members:pledge:delete",
        0,
        "patreon-a",
        "a@example.test",
        next_charge="",
        configure_authoritative=False,
    )
    first_fetch_started = asyncio.Event()
    release_first_fetch = asyncio.Event()
    fetch_count = 0

    async def ordered_provider():
        nonlocal fetch_count
        fetch_count += 1
        if fetch_count == 1:
            first_fetch_started.set()
            await release_first_fetch.wait()
            return (200, member_snapshot(amount=1000))
        return (
            200,
            member_snapshot(
                amount=0,
                next_charge="",
                patron_status="former_patron",
            ),
        )

    runtime.provider[(MEMBER_URL, "GET")] = ordered_provider

    async def scenario():
        older = asyncio.create_task(
            handler.patreon_webhook_handler(runtime, active_notification)
        )
        await first_fetch_started.wait()
        newer = await handler.patreon_webhook_handler(
            runtime, cancellation_notification
        )
        release_first_fetch.set()
        return await older, newer

    older, newer = run(scenario())
    projection = unseal(runtime.connection.execute(
        "SELECT data FROM patreon_account_links WHERE account_bi='acct-a'"
    ).fetchone()[0])

    assert decode(newer.body)["tier"] == "free"
    assert decode(older.body) == {"ok": True, "superseded": True}
    assert projection["tier"] == "free"
    assert active_roles(runtime, "acct-a") == set()
    assert runtime.connection.execute(
        "SELECT generation FROM patreon_reconciliation_generations "
        "WHERE member_bi=?",
        (bi("patreon-member-v1:member-1"),),
    ).fetchone()[0] == 2
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_webhook_events"
    ).fetchone()[0] == 1
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM sensitive_audit_log"
    ).fetchone()[0] == 1


def test_unconfigured_webhook_secret_is_expected_degraded(runtime):
    notification = webhook_request(
        runtime, "members:pledge:update", 1000, "patreon-a", "a@example.test")
    runtime.PATREON_WEBHOOK_SECRET = ""

    response = run(handler.patreon_webhook_handler(runtime, notification))

    assert response.status == 503
    assert response.headers["x-forkmesh-expected-degraded"] == "1"
    assert response.headers["cache-control"].startswith("no-store")
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_webhook_events"
    ).fetchone()[0] == 0


@pytest.mark.parametrize("failure", ["missing_token", "api_failure"])
def test_reconciliation_failure_preserves_state_and_allows_retry(runtime, failure):
    add_account(runtime, "acct-a", "a@example.test")
    runtime.connection.execute(
        "INSERT INTO patreon_account_links "
        "(account_bi,patreon_user_bi,data,linked_at,updated_at) "
        "VALUES (?,?,?,?,?)",
        ("acct-a", bi("patreon-a"), encode({"tier": "free"}), NOW, NOW),
    )
    notification = webhook_request(
        runtime,
        "members:pledge:update",
        1000,
        "patreon-a",
        "a@example.test",
        configure_authoritative=False,
    )
    if failure == "missing_token":
        runtime.PATREON_CREATOR_ACCESS_TOKEN = ""
    else:
        runtime.provider[(MEMBER_URL, "GET")] = (503, None)

    response = run(handler.patreon_webhook_handler(runtime, notification))

    assert response.status == 503
    assert (
        response.headers.get("x-forkmesh-expected-degraded") == "1"
    ) is (failure == "missing_token")
    assert active_roles(runtime, "acct-a") == set()
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_webhook_events"
    ).fetchone()[0] == 0
    projection = unseal(runtime.connection.execute(
        "SELECT data FROM patreon_account_links WHERE account_bi='acct-a'"
    ).fetchone()[0])
    assert projection["tier"] == "free"


def test_email_fallback_uses_authoritative_member_email_only(runtime):
    add_account(runtime, "acct-a", "a@example.test")
    notification = webhook_request(
        runtime,
        "members:pledge:create",
        1000,
        "patreon-a",
        "a@example.test",
        configure_authoritative=False,
    )
    runtime.provider[(MEMBER_URL, "GET")] = (
        200,
        member_snapshot(email="different@example.test"),
    )

    unmatched = run(handler.patreon_webhook_handler(runtime, notification))
    runtime.provider[(MEMBER_URL, "GET")] = (
        200,
        member_snapshot(email="a@example.test"),
    )
    matched = run(handler.patreon_webhook_handler(runtime, notification))

    assert decode(unmatched.body)["matched"] is False
    assert decode(matched.body)["matched"] is True
    assert active_roles(runtime, "acct-a") == {
        "pro_member",
        "supporting_member",
    }
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_account_links"
    ).fetchone()[0] == 0


def test_webhook_reconciliation_uses_fixed_member_path_and_creator_token(runtime):
    add_account(runtime, "acct-a", "a@example.test")

    response = run(handler.patreon_webhook_handler(
        runtime,
        webhook_request(
            runtime,
            "members:pledge:create",
            1000,
            "patreon-a",
            "a@example.test",
        ),
    ))

    assert decode(response.body)["matched"] is True
    assert runtime.provider_calls[-1] == {
        "url": MEMBER_URL,
        "method": "GET",
        "form": None,
        "access_token": "creator-access-token",
    }


@pytest.mark.parametrize("next_charge", ["", "malformed", "2029-01-01T00:00:00Z"])
def test_webhook_positive_invalid_expiry_is_rejected_without_mutation(
    runtime, next_charge
):
    add_account(runtime, "acct-a", "a@example.test")
    runtime.connection.execute(
        "INSERT INTO patreon_account_links "
        "(account_bi,patreon_user_bi,data,linked_at,updated_at) "
        "VALUES (?,?,?,?,?)",
        ("acct-a", bi("patreon-a"), encode({"tier": "pro"}), NOW, NOW),
    )
    runtime.connection.execute(
        "INSERT INTO role_grants VALUES (?,?,?,?,?,?,?,?)",
        (
            "acct-a",
            "pro_member",
            "platform",
            "",
            "acct-a",
            NOW,
            NOW + 60_000,
            0,
        ),
    )
    before_user = runtime.connection.execute(
        "SELECT data FROM users WHERE user_bi='acct-a'"
    ).fetchone()[0]
    before_link = runtime.connection.execute(
        "SELECT data FROM patreon_account_links WHERE account_bi='acct-a'"
    ).fetchone()[0]

    response = run(handler.patreon_webhook_handler(
        runtime,
        webhook_request(
            runtime,
            "members:pledge:update",
            1000,
            "patreon-a",
            "a@example.test",
            next_charge=next_charge,
        ),
    ))

    assert response.status == 400
    assert decode(response.body)["error"] == "invalid_membership_expiry"
    assert runtime.connection.execute(
        "SELECT data FROM users WHERE user_bi='acct-a'"
    ).fetchone()[0] == before_user
    assert runtime.connection.execute(
        "SELECT data FROM patreon_account_links WHERE account_bi='acct-a'"
    ).fetchone()[0] == before_link
    assert active_roles(runtime, "acct-a") == {"pro_member"}
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_webhook_events"
    ).fetchone()[0] == 0
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM sensitive_audit_log"
    ).fetchone()[0] == 0


def test_provider_json_aborts_chunked_response_over_256_kib(runtime, monkeypatch):
    response = ChunkedProviderResponse([
        b'{"data":"',
        b"x" * (128 * 1024),
        b"y" * (129 * 1024),
        b'"}',
    ])

    async def fake_fetch(*_args, **_kwargs):
        return response

    monkeypatch.setitem(
        handler.__dict__, "js_fetch_with_timeout", fake_fetch
    )

    status, payload = run(REAL_PROVIDER_JSON(
        handler.PATREON_TOKEN_URL,
        method="POST",
        form={"code": "provider-code"},
    ))

    assert status == 502
    assert payload is None
    assert response.body.reader.cancelled is True


def test_provider_json_uses_fixed_bounded_member_request(runtime, monkeypatch):
    response = ChunkedProviderResponse([encode(member_snapshot()).encode()])
    captured = {}

    async def fake_fetch(url, options, timeout):
        captured.update({"url": url, "options": options, "timeout": timeout})
        return response

    monkeypatch.setitem(
        handler.__dict__, "js_fetch_with_timeout", fake_fetch
    )

    status, payload = run(REAL_PROVIDER_JSON(
        MEMBER_URL,
        method="GET",
        access_token="creator-access-token",  # forkmesh-secret-scan:ignore-line
    ))

    assert status == 200
    assert payload["data"]["id"] == "member-1"
    assert captured["url"] == (
        MEMBER_URL
        + "?include=user,campaign&fields%5Bmember%5D="
        + "currently_entitled_amount_cents,email,patron_status,next_charge_date"
        + "&fields%5Buser%5D=email"
    )
    assert captured["options"]["redirect"] == "manual"
    assert captured["options"]["headers"]["authorization"] == (
        "Bearer creator-access-token"
    )
    assert captured["timeout"] == handler.PROVIDER_TIMEOUT_SECONDS


def test_account_lifecycle_helpers_mutate_all_patreon_state(runtime):
    add_account(runtime, "acct-old")
    runtime.connection.execute(
        "INSERT INTO patreon_oauth_states VALUES (?,?,?,?,?)",
        ("state", "acct-old", "encrypted", NOW, NOW + 1),
    )
    runtime.connection.execute(
        "INSERT INTO patreon_account_links VALUES (?,?,?,?,?)",
        ("acct-old", bi("patreon-a"), "encrypted", NOW, NOW),
    )
    runtime.connection.execute(
        "INSERT INTO patreon_webhook_events VALUES (?,?,?,?)",
        ("event", NOW, "members:pledge:update", "acct-old"),
    )
    runtime.connection.execute(
        "INSERT INTO role_grants VALUES (?,?,?,?,?,?,?,?)",
        (
            "acct-old",
            "pro_member",
            "platform",
            "",
            "acct-old",
            NOW,
            NOW + 1,
            0,
        ),
    )

    run(handler.patreon_account_renamed(runtime, "acct-old", "acct-new"))

    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_oauth_states"
    ).fetchone()[0] == 0
    assert runtime.connection.execute(
        "SELECT account_bi FROM patreon_account_links"
    ).fetchone()[0] == "acct-new"
    assert runtime.connection.execute(
        "SELECT account_bi FROM patreon_webhook_events"
    ).fetchone()[0] == "acct-new"
    assert runtime.connection.execute(
        "SELECT account_bi FROM role_grants"
    ).fetchone()[0] == "acct-new"

    run(handler.patreon_account_deleted(runtime, "acct-new"))

    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM patreon_account_links"
    ).fetchone()[0] == 0
    assert runtime.connection.execute(
        "SELECT account_bi FROM patreon_webhook_events"
    ).fetchone()[0] == ""
    assert runtime.connection.execute(
        "SELECT COUNT(*) FROM role_grants"
    ).fetchone()[0] == 0

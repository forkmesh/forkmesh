"""Patreon OAuth account-linking security and routing contracts."""

import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY = (
    (SRC / "entry.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (SRC / "app_runtime.py").read_text(encoding="utf-8")
)
HANDLER = (SRC / "patreon_handler.py").read_text(encoding="utf-8")
SCHEMA = (SRC / "schema.py").read_text(encoding="utf-8")
URLS = (SRC / "urls.py").read_text(encoding="utf-8")

SPEC = importlib.util.spec_from_file_location("patreon", SRC / "patreon.py")
patreon = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(patreon)


def test_oauth_routes_are_registered_through_accounts_and_callback_dispatch():
    assert 'url.path == "/api/accounts/patreon"' in ENTRY
    assert 'url.path == "/api/accounts/patreon/oauth/start"' in ENTRY
    assert "patreon_connection_handler" in ENTRY
    assert "patreon_oauth_start_handler" in ENTRY
    assert "PATREON_OAUTH_CALLBACK_RE" in URLS
    assert r"^/api/integrations/patreon/callback/?$" in URLS
    assert "PATREON_OAUTH_CALLBACK_RE.match(url.path)" in ENTRY
    assert "patreon_oauth_callback_handler" in ENTRY
    for handler_name in (
        "patreon_connection_handler",
        "patreon_oauth_start_handler",
        "patreon_oauth_callback_handler",
        "patreon_webhook_handler",
    ):
        body = HANDLER.split(f"async def {handler_name}", 1)[1].split(
            "\nasync def", 1
        )[0]
        assert "_bind_entry_symbols()" in body


def test_oauth_state_and_link_schema_are_encrypted_and_blind_indexed():
    assert "CREATE TABLE IF NOT EXISTS patreon_oauth_states" in SCHEMA
    assert "state_hash TEXT PRIMARY KEY" in SCHEMA
    assert "account_bi TEXT NOT NULL" in SCHEMA
    assert "data TEXT NOT NULL" in SCHEMA
    assert "idx_patreon_oauth_states_account_expiry" in SCHEMA
    assert "CREATE TABLE IF NOT EXISTS patreon_account_links" in SCHEMA
    assert "account_bi TEXT PRIMARY KEY" in SCHEMA
    assert "patreon_user_bi TEXT NOT NULL UNIQUE" in SCHEMA


def test_identity_parser_requires_requested_campaign_membership():
    payload = {
        "data": {
            "type": "user",
            "id": "patreon-user-1",
            "relationships": {
                "memberships": {
                    "data": [
                        {"type": "member", "id": "member-wrong"},
                        {"type": "member", "id": "member-right"},
                    ]
                }
            },
        },
        "included": [
            {
                "type": "member",
                "id": "member-wrong",
                "attributes": {
                    "currently_entitled_amount_cents": 5000,
                    "patron_status": "active_patron",
                },
                "relationships": {
                    "campaign": {"data": {"type": "campaign", "id": "wrong"}}
                },
            },
            {
                "type": "member",
                "id": "member-right",
                "attributes": {
                    "currently_entitled_amount_cents": 1000,
                    "patron_status": "active_patron",
                    "next_charge_date": "2026-08-30T10:00:00Z",
                },
                "relationships": {
                    "campaign": {"data": {"type": "campaign", "id": "campaign-1"}}
                },
            },
        ],
    }

    assert patreon.parse_oauth_identity(payload, "wrong-campaign") is None
    assert patreon.parse_oauth_identity(payload, "campaign-1") == {
        "patreon_user_id": "patreon-user-1",
        "amount_cents": 1000,
        "patron_status": "active_patron",
        "next_charge_at": "2026-08-30T10:00:00Z",
    }


def test_live_campaign_amounts_map_to_inherited_membership_roles():
    assert patreon._tier_from_amount_cents(0) == "free"
    assert patreon._tier_from_amount_cents(500) == "supporter"
    assert patreon._tier_from_amount_cents(1000) == "pro"
    assert patreon._tier_from_amount_cents(5000) == "builder"
    assert patreon._tier_from_amount_cents(10000) == "studio"
    assert patreon._roles_for_tier("studio") == (
        "studio_member",
        "builder_member",
        "pro_member",
        "supporting_member",
    )


def test_identity_parser_rejects_unrelated_included_membership():
    payload = {
        "data": {
            "type": "user",
            "id": "patreon-user-1",
            "relationships": {"memberships": {"data": []}},
        },
        "included": [{
            "type": "member",
            "id": "not-related",
            "attributes": {"currently_entitled_amount_cents": 1000},
            "relationships": {
                "campaign": {"data": {"type": "campaign", "id": "campaign-1"}}
            },
        }],
    }
    assert patreon.parse_oauth_identity(payload, "campaign-1") is None


def test_oauth_start_requires_auth_same_origin_and_ten_minute_encrypted_state():
    assert "async def patreon_oauth_start_handler" in HANDLER
    assert "_account_session_record(env, request, data)" in HANDLER
    assert "_request_same_origin(request)" in HANDLER
    assert "unsupported_oauth_start_field" in HANDLER
    assert "OAUTH_STATE_TTL_MS = 10 * 60 * 1000" in HANDLER
    assert "_random_bytes(32)" in HANDLER
    assert "hashlib.sha256" in HANDLER
    assert "await encrypt_row(env, state_record)" in HANDLER
    assert "INSERT INTO patreon_oauth_states" in HANDLER
    assert "PATREON_OAUTH_REDIRECT_URI" in HANDLER
    assert "PATREON_OAUTH_CALLBACK_URL" not in HANDLER


def test_callback_consumes_state_with_cas_and_checks_account_session_binding():
    assert "async def patreon_oauth_callback_handler" in HANDLER
    assert "UPDATE patreon_oauth_states SET data=?,expires_at=0" in HANDLER
    assert "WHERE state_hash=? AND data=? AND expires_at=?" in HANDLER
    assert "hmac.compare_digest" in HANDLER
    assert "account_sessions WHERE account_bi=?" in HANDLER
    assert "AND session_id=? AND revoked_at=0 AND expires_at>?" in HANDLER
    for outcome in (
        '"expired"', '"replayed"', '"mismatch"', '"wrong_campaign"', '"failed"',
    ):
        assert outcome in HANDLER


def test_provider_requests_use_fixed_paths_manual_redirects_and_bounded_bodies():
    assert 'PATREON_AUTHORIZE_URL = "https://www.patreon.com/oauth2/authorize"' in HANDLER
    assert 'PATREON_TOKEN_URL = "https://www.patreon.com/api/oauth2/token"' in HANDLER
    assert (
        'PATREON_IDENTITY_URL = "https://www.patreon.com/api/oauth2/v2/identity"'
        in HANDLER
    )
    assert '"redirect": "manual"' in HANDLER
    assert "MAX_PROVIDER_RESPONSE_BYTES" in HANDLER
    assert "content-length" in HANDLER


def test_callback_has_fixed_safe_outcomes_and_never_persists_tokens():
    assert '"/dashboard/settings/account?patreon=" + outcome' in HANDLER
    assert "PATREON_CALLBACK_OUTCOMES" in HANDLER
    assert "access_token" in HANDLER
    assert "refresh_token" not in HANDLER
    for sql_fragment in (
        "INSERT INTO patreon_account_links",
        "INSERT INTO patreon_oauth_states",
    ):
        statement_tail = HANDLER.split(sql_fragment, 1)[1][:1200]
        assert "accessToken" not in statement_tail


def test_duplicate_identity_is_conflict_safe_and_projection_is_private():
    assert "patreon_user_bi TEXT NOT NULL UNIQUE" in SCHEMA
    assert "duplicate_identity" in HANDLER
    assert '"connected": bool(row)' in HANDLER
    assert "patreon_user_bi" not in HANDLER.split(
        "async def patreon_connection_handler", 1
    )[1].split("async def", 1)[0].split("json_response", 1)[-1]


def test_account_lifecycle_moves_or_removes_patreon_state_without_identifiers():
    rename = ENTRY.split("async def _rename_account_namespace", 1)[1].split(
        "async def _delete_bounties_namespace", 1
    )[0]
    delete = ENTRY.split("async def _delete_account_namespace", 1)[1].split(
        "def _owned_nodes", 1
    )[0]
    assert "patreon_account_renamed(env, name_bi, new_name_bi)" in rename
    assert "patreon_account_deleted(env, name_bi)" in delete

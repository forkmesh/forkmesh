#!/usr/bin/env python3
"""Password users originate at /signup; verification requires a real POST."""

import ast
import base64
import hmac
import re
from pathlib import Path
from types import SimpleNamespace

import tomllib


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src/entry.py"
ROUTE_HANDLERS = ROOT / "src/route_handlers.py"
SCHEMA = ROOT / "src/schema.py"
MIGRATION = ROOT / "migrations/0138_account_signup_intents.sql"
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8") + "\n\n\n"
    + ENTRY.with_name("app_runtime.py").read_text(encoding="utf-8")
)
ROUTE_TEXT = ROUTE_HANDLERS.read_text(encoding="utf-8")
SCHEMA_TEXT = SCHEMA.read_text(encoding="utf-8")


def _selected_entry_namespace():
    wanted_functions = {
        "_account_signup_intent_signature",
        "_account_signup_intent_value",
        "_account_signup_intent_claim",
        "_email_verification_issued_at",
        "_email_verify_link_token",
        "_email_verify_link_parts",
        "_email_verify_masked",
        "_signup_page_target",
    }
    wanted_assignments = {
        "ACCOUNT_SIGNUP_INTENT_COOKIE",
        "ACCOUNT_SIGNUP_INTENT_TTL_MS",
        "ACCOUNT_SIGNUP_INTENT_FUTURE_SKEW_MS",
        "EMAIL_VERIFICATION_TOKEN_TTL_MS",
        "EMAIL_VERIFICATION_FUTURE_SKEW_MS",
        "EMAIL_VERIFY_LINK_TOKEN_RE",
    }
    selected = []
    for node in ast.parse(ENTRY_TEXT, filename=str(ENTRY)).body:
        if (isinstance(node, ast.Assign)
                and any(isinstance(target, ast.Name)
                        and target.id in wanted_assignments
                        for target in node.targets)):
            selected.append(node)
        elif (isinstance(node, ast.FunctionDef)
              and node.name in wanted_functions):
            selected.append(node)
    namespace = {
        "Date": SimpleNamespace(now=lambda: 1_700_000_000_000),
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "base64": base64,
        "hmac": hmac,
        "re": re,
        "_require_data_secret": lambda _env: "test-data-secret",
        "_cookie_value": lambda request, name: request.cookies.get(name, ""),
    }
    module = ast.fix_missing_locations(ast.Module(selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


NS = _selected_entry_namespace()


def test_signup_intent_cookie_is_signed_bounded_and_expires():
    now = 1_700_000_000_000
    ttl = NS["ACCOUNT_SIGNUP_INTENT_TTL_MS"]
    claim_id = "a" * 36
    value = NS["_account_signup_intent_value"](
        object(), claim_id, now, now + ttl)
    request = SimpleNamespace(cookies={
        NS["ACCOUNT_SIGNUP_INTENT_COOKIE"]: value,
    })
    claim = NS["_account_signup_intent_claim"](object(), request, now=now)
    assert claim == {
        "claimId": claim_id,
        "issuedAt": now,
        "expiresAt": now + ttl,
    }

    request.cookies[NS["ACCOUNT_SIGNUP_INTENT_COOKIE"]] = value[:-1] + "0"
    assert NS["_account_signup_intent_claim"](
        object(), request, now=now) is None
    request.cookies[NS["ACCOUNT_SIGNUP_INTENT_COOKIE"]] = value
    assert NS["_account_signup_intent_claim"](
        object(), request, now=now + ttl + 1) is None


def test_direct_signup_is_rejected_and_claim_is_consumed_before_write():
    body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_signup(env, request):"):
        ENTRY_TEXT.index("async def _account_password_record")
    ]
    assert body.index("_account_signup_intent_claim") < body.index(
        "bounded_json_request")
    assert "_account_signup_intent_available" in body
    assert "_signup_page_required_response" in body
    assert body.index("_consume_account_signup_intent") < body.index(
        "_save_account")
    assert "RETURNING claim_id" in ENTRY_TEXT[
        ENTRY_TEXT.index("async def _consume_account_signup_intent"):
        ENTRY_TEXT.index("def _signup_page_required_response")
    ]


def test_signup_claim_is_httponly_one_use_state_with_no_raw_identity_data():
    helper_body = ENTRY_TEXT[
        ENTRY_TEXT.index("ACCOUNT_SIGNUP_INTENT_COOKIE"):
        ENTRY_TEXT.index("async def _account_signup(env, request):")
    ]
    assert "HttpOnly" in helper_body
    assert "SameSite=Strict" in helper_body
    assert "Path=/api/accounts/signup" in helper_body
    assert "consumed_at=0" in helper_body
    assert "UPDATE account_signup_intents SET consumed_at" in helper_body
    table = SCHEMA_TEXT[
        SCHEMA_TEXT.index("CREATE TABLE IF NOT EXISTS account_signup_intents"):
        SCHEMA_TEXT.index("CREATE INDEX IF NOT EXISTS idx_account_signup_intents_expiry")
    ]
    assert "email_field_name TEXT NOT NULL" in table
    for sensitive_column in ("email_bi", "ip_bi", "user_agent", "password"):
        assert sensitive_column not in table


def test_signup_page_is_worker_owned_and_serves_the_static_form_with_claim():
    manifest = tomllib.loads((ROOT / "wrangler.toml").read_text(encoding="utf-8"))
    routes = manifest["assets"]["run_worker_first"]
    assert "/signup" in routes
    assert "/signup/" in routes
    assert "!/signup/" not in routes
    route_body = ENTRY_TEXT[
        ENTRY_TEXT.index('if url.path == "/signup/":'):
        ENTRY_TEXT.index('if url.path in ("/api", "/api/", "/developers", "/developers/")')
    ]
    assert 'headers={"location": signup_target}' in route_body
    assert "_serve_signup_page(request, url)" in route_body
    serve_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _serve_signup_page"):
        ENTRY_TEXT.index("async def _git_host")
    ]
    assert "_issue_account_signup_intent" in serve_body
    assert 'ASSETS.fetch(base + "signup.html")' in serve_body
    assert '"set-cookie": _account_signup_intent_cookie' in serve_body
    assert 'body.replace(marker, email_field_name, 1)' in serve_body
    assert "_admin_signup_referral_code_mode" in serve_body
    assert "__FORKMESH_SIGNUP_REFERRAL_CODE_VISIBILITY__" in serve_body
    assert '"cache-control": "no-store, max-age=0, must-revalidate"' in serve_body


def test_signup_intent_schema_and_latest_migration_match():
    assert "CREATE TABLE IF NOT EXISTS account_signup_intents" in SCHEMA_TEXT
    assert "idx_account_signup_intents_expiry" in SCHEMA_TEXT
    migration = MIGRATION.read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS account_signup_intents" in migration
    assert "idx_account_signup_intents_expiry" in migration
    # The migration stamp must be a real native schema fingerprint, not an
    # arbitrary label that readiness checks could mistake for current schema.
    assert re.search(r"schema_fingerprint:[0-9a-f]{64}", migration)


def test_reserve_and_finalize_cannot_create_a_keyless_web_user():
    reserve = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_reserve"):
        ENTRY_TEXT.index("def _amount_sol")
    ]
    assert "if not valid_node_pubkey(pubkey):" in reserve
    assert "desktop_node_proof_required" in reserve
    assert "ed25519_verify(pubkey, signature, canonical)" in reserve

    finalize = ROUTE_TEXT[
        ROUTE_TEXT.index("async def _account_finalize"):
        ROUTE_TEXT.index("async def _solana_transaction_matches_plan")
    ]
    assert 'if not rec.get("pubkey"):' in finalize
    assert "_signup_page_required_response()" in finalize
    assert 'rec["kind"] = "node"' in finalize
    assert 'rec["kind"] = "user"' not in finalize


def test_email_get_is_read_only_and_only_post_confirms():
    verify = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _verify_email(env, request):"):
        ENTRY_TEXT.index("# --- Relay federation")
    ]
    assert 'if method == "GET":' in verify
    assert '<form method=\\"post\\"' in verify
    assert "No account change occurs until this button is " in verify
    assert '"pressed.</small></p>"' in verify
    assert verify.index('if method == "GET":') < verify.index(
        'rec["email_verified"] = True')
    assert 'rec["email_verification_method"] = "recipient_link_post"' in verify
    assert "email_verified_at" in verify

    admin = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _admin_verify_email"):
        ENTRY_TEXT.index("async def _admin_delete_unverified_account")
    ]
    # A proven administrator can close out a signup the mailbox never
    # confirmed -- delivery is not always possible -- but the override
    # must never be recorded as the recipient having pressed the link.
    assert "_admin_authorized(env, node, ts, sig, canonical)" in admin
    assert 'rec["email_verified"] = True' in admin
    assert 'rec["email_verification_method"] = "admin_override"' in admin
    assert 'rec["email_verified_by"] = node' in admin
    assert 'rec["email_verification_method"] = "recipient_link_post"' \
        not in admin
    assert '{"method": "admin_override", "alreadyVerified": already}' \
        in admin
    assert "_save_account" in admin


def test_verification_links_expire_and_bind_the_issued_time():
    now = 1_700_000_000_000
    ttl = NS["EMAIL_VERIFICATION_TOKEN_TTL_MS"]
    issued = NS["_email_verification_issued_at"]
    assert issued(now, now=now) == now
    assert issued(now - ttl, now=now) == now - ttl
    assert issued(now - ttl - 1, now=now) == 0
    assert issued(now + NS["EMAIL_VERIFICATION_FUTURE_SKEW_MS"] + 1,
                  now=now) == 0
    token_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _email_verify_token"):
        ENTRY_TEXT.index("def _public_base_url")
    ]
    assert "forkmesh-email-verify-v2" in token_body
    assert "issued_at" in token_body


def test_verification_link_is_one_opaque_token_that_resists_edits():
    make = NS["_email_verify_link_token"]
    parts = NS["_email_verify_link_parts"]
    issued = 1_700_000_000_123
    mac = bytes(range(32))
    token = make(issued, mac)
    assert len(token) == 51
    assert str(issued) not in token
    assert re.fullmatch(r"[A-Za-z0-9_-]+", token)
    assert parts(token) == (issued, mac.hex())
    assert parts(token[:-1]) == (0, "")
    assert parts(token + "A") == (0, "")
    assert parts("x" * 50 + "!") == (0, "")
    flipped = ("B" if token[3] == "A" else "A")
    edited_issued, edited_mac = parts(token[:3] + flipped + token[4:])
    assert (edited_issued, edited_mac) != (issued, mac.hex())

    masked = NS["_email_verify_masked"]
    assert masked("jett@forkmesh.com") == "j•••@forkmesh.com"
    assert masked("no-at-sign") == ""

    send = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _send_verification_email"):
        ENTRY_TEXT.index("# --- \"Send it again\"")
    ]
    assert "&issuedAt=" not in send
    assert "_email_verify_link(env, name, email, issued_at)" in send
    assert '{"email_verify_issued_at": issued_at} if ok else None' in send

    verify = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _verify_email(env, request):"):
        ENTRY_TEXT.index("# --- Relay federation")
    ]
    assert 'latest and issued_at != latest' in verify
    assert "This link was replaced" in verify
    assert "Email already confirmed" in verify
    assert 'rec.pop("email_verify_issued_at", None)' in verify
    assert verify.index("Email already confirmed") < verify.index(
        'rec["email_verified"] = True')


def test_frontend_sends_the_page_cookie_and_explains_expiry():
    signup_html = (ROOT / "public/signup.html").read_text(encoding="utf-8")
    signup_js = (ROOT / "public/signup.js").read_text(encoding="utf-8")
    assert 'name="email"' in signup_html
    assert 'name="__FORKMESH_SIGNUP_EMAIL_FIELD__"' in signup_html
    assert "hidden" in signup_html[
        signup_html.index('name="email"'):
        signup_html.index('name="__FORKMESH_SIGNUP_EMAIL_FIELD__"')
    ]
    assert 'credentials: "same-origin"' in signup_js
    assert 'email: ""' in signup_js
    assert '[emailInput.name]: email' in signup_js
    assert 'signupReferralCode: signupReferralCodeInput' in signup_js
    assert 'body.error === "signup_page_required"' in signup_js
    assert "Reload /signup and try again" in signup_js
    assert "invalid_signup_referral_code" in signup_js


def test_honeypot_fails_silently_and_random_field_is_the_only_email_input():
    body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_signup(env, request):"):
        ENTRY_TEXT.index("async def _account_password_record")
    ]
    honeypot = body[
        body.index('if clean_string(data.get("email", ""), 254).strip():'):
        body.index("email_field_name = clean_string")
    ]
    assert "_consume_account_signup_intent" in honeypot
    assert '{"ok": True, "accepted": True}' in honeypot
    assert "_save_account" not in honeypot
    assert 'data.get(email_field_name, "")' in body
    assert 'data.get("email", ""), 254).strip().lower()' not in body


def test_world_and_mobile_send_account_creation_to_the_signup_page():
    repository = ROOT.parent
    world_js = (
        repository / "world/public/world/world.js"
    ).read_text(encoding="utf-8")
    ios_auth = (
        repository / "native/ios/ForkMeshCore/Sources/FMAccount/AuthService.swift"
    ).read_text(encoding="utf-8")
    ios_flow = (
        repository / "native/ios/ForkMesh/Features/Account/AuthFlowView.swift"
    ).read_text(encoding="utf-8")
    android_auth = (
        repository
        / "native/android/core/src/main/kotlin/com/forkmesh/nativeapp/api/AuthController.kt"
    ).read_text(encoding="utf-8")
    android_flow = (
        repository
        / "native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/account/auth/AuthFlow.kt"
    ).read_text(encoding="utf-8")

    assert 'data-world-signup-link href="/signup"' in world_js
    assert '"/api/accounts/signup"' not in world_js
    assert "submitWorldSignup" not in world_js
    assert 'public var signupUrl: URL? { base("/signup") }' in ios_auth
    assert '"/api/accounts/signup"' not in ios_auth
    assert "Open secure signup" in ios_flow
    assert 'val signupUrl: String get() = base("/signup")' in android_auth
    assert '"/api/accounts/signup"' not in android_auth
    assert "Open secure signup" in android_flow
    assert "it.signupUrl" in android_flow


def test_challenge_replay_post_reaches_the_form_not_a_json_error():
    """A bot-challenge replay POSTs the blocked navigation back to /signup.

    Once its token is spent the edge stops absorbing that replay, so the page
    itself has to survive a POST it never authored. Answering with a JSON 405
    painted {"error":"method_not_allowed"} over the signup form.
    """
    route_body = ENTRY_TEXT[
        ENTRY_TEXT.index('if url.path == "/signup/":'):
        ENTRY_TEXT.index(
            'if url.path in ("/api", "/api/", "/developers", "/developers/")')
    ]
    assert '{"error": "method_not_allowed"}' not in route_body
    assert "status=405" not in route_body
    assert "status=303" in route_body
    assert '"location": _signup_page_target(url.query)' in route_body


def test_signup_redirect_drops_challenge_tokens_and_keeps_referral():
    target = NS["_signup_page_target"]
    assert target("") == "/signup"
    assert target(None) == "/signup"
    # The replay parameters are throwaway state; they must not reach the
    # address bar or seed a second replay.
    assert target("__cf_chl_f_tk=abc.def-123") == "/signup"
    assert target("__cf_chl_tk=xyz&__cf_chl_rt_tk=q") == "/signup"
    # Referral attribution is the one query pair signup.js reads, and it is
    # kept byte-for-byte so a referred visitor still credits their referrer.
    assert target("ref=ada-lovelace&rid=deadbeef") == (
        "/signup?ref=ada-lovelace&rid=deadbeef")
    assert target("__cf_chl_f_tk=abc&ref=ada&rid=99") == "/signup?ref=ada&rid=99"
    assert target("ref=a%20b") == "/signup?ref=a%20b"

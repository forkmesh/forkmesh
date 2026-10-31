#!/usr/bin/env python3
"""Issue #592 — unverified accounts are held out of the product.

Signup hands the new account a session immediately, and the "Check your inbox"
screen used to offer a plain ``<a href="/dashboard">Continue to dashboard</a>``:
an address nobody had proven control of walked straight into the authenticated
app, so automated signups reached the write APIs in bulk.

Contract under test, read straight off the sources:
- ``_account_email_unverified`` names exactly the held-back records — a web
  account with an address on file that is not confirmed — and never a key-bound
  node or an account with no address to confirm;
- ``_account_session_record`` refuses every state-changing call such a session
  authorizes, and leaves safe reads alone so the pending screen can render;
- endpoints an unverified account must still reach (resend the link, delete
  itself) opt out explicitly with ``allow_unverified``;
- ``POST /api/accounts/profile`` re-checks per field, because it has a second,
  password-authenticated entry that never touches the session gate;
- the router turns the refusal into one 403 ``email_not_verified`` instead of a
  5xx, and the frontend routes the signup button and every dashboard document
  to the pending screen rather than to /dashboard.
"""

import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
PUBLIC = ROOT / "public"
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8") + "\n\n\n"
    + ENTRY.with_name("app_runtime.py").read_text(encoding="utf-8")
)

FUNCS = {
    "_account_kind",
    "_account_email_unverified",
    "_email_not_verified_response",
    "_account_session_record",
}
ASSIGNS = {
    "UNVERIFIED_SAFE_METHODS",
    "UNVERIFIED_EMAIL_ERROR",
    "UNVERIFIED_PROFILE_FIELDS",
    "VERIFY_PENDING_PATH",
}


def _load():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and {
            t.id for t in node.targets if isinstance(t, ast.Name)
        } & ASSIGNS:
            selected.append(node)
        elif (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in FUNCS):
            selected.append(node)
    found = {getattr(n, "name", "") for n in selected}
    assert FUNCS <= found
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = {
        "clean_string": lambda value, maximum=240: str(value or "")[:maximum],
        "json_response": lambda body, status=200, **_kwargs: {
            "status": status, "body": body},
        "method_name": lambda request: request.method,
        "_request_account_session_token": lambda request, payload=None: (
            request.token, False),
        "_request_origin_allowed": lambda _env, _request: True,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


NS = _load()


def _request(method="POST", token="tok"):
    return SimpleNamespace(method=method, token=token, headers={})


def _session(rec, method="POST", **kwargs):
    async def _lookup(_env, token, touch=True):
        return ("bi:alice", rec, "sid") if token else ("", None, "")

    NS["_account_session_lookup"] = _lookup
    return asyncio.run(
        NS["_account_session_record"](object(), _request(method), **kwargs))


UNVERIFIED = {"name": "alice", "kind": "user", "email": "a@example.com",
              "email_verified": False, "pass_hash": "x"}
VERIFIED = {**UNVERIFIED, "email_verified": True}


# --- who counts as unverified ----------------------------------------------

def test_only_web_accounts_with_an_unconfirmed_address_are_held_back():
    unverified = NS["_account_email_unverified"]
    assert unverified(UNVERIFIED) is True
    assert unverified(VERIFIED) is False
    # Nothing to confirm: a free key-bound join can carry no address at all.
    assert unverified({**UNVERIFIED, "email": ""}) is False
    # Key-bound nodes authorize by signature, not by a confirmed inbox.
    assert unverified({**UNVERIFIED, "kind": "node"}) is False
    assert unverified(None) is False


# --- the session choke point ------------------------------------------------

def test_an_unverified_session_cannot_authorize_a_write():
    for method in ("POST", "PUT", "PATCH", "DELETE"):
        try:
            _session(UNVERIFIED, method=method)
        except RuntimeError as error:
            assert str(error) == NS["UNVERIFIED_EMAIL_ERROR"]
        else:
            raise AssertionError(method + " was allowed through")


def test_reads_stay_open_so_the_pending_screen_can_render():
    for method in NS["UNVERIFIED_SAFE_METHODS"]:
        assert _session(UNVERIFIED, method=method) == ("bi:alice", UNVERIFIED)


def test_a_verified_session_writes_exactly_as_before():
    assert _session(VERIFIED) == ("bi:alice", VERIFIED)


def test_lifecycle_endpoints_opt_out_explicitly():
    # resend-verification and the profile POST pass allow_unverified: being
    # unverified is the precondition for the first and the second still has to
    # let the account delete itself.
    assert _session(UNVERIFIED, allow_unverified=True) == ("bi:alice", UNVERIFIED)
    assert "allow_unverified=True)" in ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_resend_verification"):
        ENTRY_TEXT.index("async def _account_tip_notice")
    ]


def test_a_guest_is_still_unauthorized_not_unverified():
    assert _session(None) == ("", None)


# --- the refusal the clients read -------------------------------------------

def test_the_refusal_is_a_403_pointing_at_the_pending_screen():
    response = NS["_email_not_verified_response"]()
    assert response["status"] == 403
    assert response["body"]["error"] == "email_not_verified"
    assert response["body"]["verifyUrl"] == NS["VERIFY_PENDING_PATH"]


def test_the_router_answers_the_refusal_instead_of_logging_a_5xx():
    router = ENTRY_TEXT[ENTRY_TEXT.index("    async def _fetch_inner"):]
    assert "elif str(error) == UNVERIFIED_EMAIL_ERROR:" in router
    assert "response = _email_not_verified_response()" in router


# --- the password-authenticated profile path --------------------------------

def test_the_profile_post_regates_per_field():
    # Knowing the account password must not buy a profile edit: that branch
    # never reaches the session gate, so _account_profile checks `rec` itself.
    profile_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_profile"):
        ENTRY_TEXT.index("async def _account_claim_node")
    ]
    assert ("if _account_email_unverified(rec) and (\n"
            "            set(data.keys()) - UNVERIFIED_PROFILE_FIELDS):\n"
            "        return _email_not_verified_response()") in profile_body
    allowed = NS["UNVERIFIED_PROFILE_FIELDS"]
    # Leaving and re-requesting the link are the only writes still permitted.
    assert {"deleteAccount", "disableAccount", "resendVerification"} <= allowed
    # Public profile surface — what a bulk fake signup wants — is not.
    assert not ({"profileBio", "avatarPng", "profileLinks", "solana",
                 "newNodeName"} & allowed)


# --- the frontend hold-out --------------------------------------------------

def test_signup_no_longer_links_the_unverified_account_to_the_dashboard():
    signup_html = (PUBLIC / "signup.html").read_text(encoding="utf-8")
    signup_js = (PUBLIC / "signup.js").read_text(encoding="utf-8")

    assert 'href="/dashboard" id="verify-continue"' not in signup_html
    assert "Continue to dashboard" not in signup_html
    assert 'href="/verify-pending" id="verify-continue"' in signup_html
    # Even the missing-markup fallback lands on the pending screen. Only an
    # account created without an address (nothing to confirm) continues to
    # the dashboard, through one function.
    free = signup_js[signup_js.index("function openEmailFreeAccount()"):]
    free = free[:free.index("\n  }\n") + 4]
    assert signup_js.count('"/dashboard"') == free.count('"/dashboard"') == 1
    verify = signup_js[signup_js.index("function showVerifyView("):]
    assert "openEmailFreeAccount" not in verify[:verify.index("\n  }\n")]
    assert 'location.href = "/verify-pending"' in signup_js


def test_the_pending_screen_only_needs_unverified_safe_calls():
    pending_html = (PUBLIC / "verify-pending.html").read_text(encoding="utf-8")
    pending_js = (PUBLIC / "verify-pending.js").read_text(encoding="utf-8")

    assert "Pending verification" in pending_html
    # It reads the public account record (GET) to notice the confirmation and
    # posts only to resend-verification/logout — all reachable while unverified.
    assert '"/api/accounts/" + encodeURIComponent(name)' in pending_js
    assert '"/api/accounts/resend-verification"' in pending_js
    assert '"/api/accounts/logout"' in pending_js
    # The dashboard is reached only after the server says the address is good.
    assert 'if (body.exists === false || !body.emailVerified) return false;' in pending_js


def test_every_dashboard_document_bounces_an_unverified_session():
    dashboard_js = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")

    # `=== false` on purpose: a stored session written before the flag existed
    # carries no opinion, and the canonical read settles it a moment later.
    assert ("return Boolean(session?.email) && session?.emailVerified === false;"
            in dashboard_js)
    assert 'location.replace(VERIFY_PENDING_PATH);' in dashboard_js
    assert "if (bounceToVerification(session)) return false;" in dashboard_js
    assert "if (bounceToVerification(nextSession)) return;" in dashboard_js

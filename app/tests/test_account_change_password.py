"""A signed-in account can change its own password (issue #648).

Settings used to expose five "Current password" inputs, every one of them a
confirmation for some *other* action, and the only route to a new password was
to sign out and wait for a reset email. These tests pin the authenticated
change: it re-verifies the current password, has its own brute-force budget,
signs the other devices out, and keeps the calling device signed in.
"""

import ast
import asyncio
import copy
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
PASSWORD_RESET = ROOT / "src" / "password_reset.py"
PASSWORD_RESET_TEXT = PASSWORD_RESET.read_text(encoding="utf-8")
PUBLIC = ROOT / "public"

LOADED = (
    "PASSWORD_CHANGE_MIN_LENGTH",
    "_password_change_attempt_key",
    "_send_password_changed_notice",
    "_account_change_password",
)


def _load(namespace):
    """Exec exactly the change-password family against a fake runtime."""
    tree = ast.parse(PASSWORD_RESET_TEXT, filename=str(PASSWORD_RESET))
    nodes = []
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in LOADED:
                nodes.append(node)
        elif isinstance(node, ast.Assign):
            targets = [t.id for t in node.targets if isinstance(t, ast.Name)]
            if any(name in LOADED for name in targets):
                nodes.append(node)
    assert len(nodes) == len(LOADED), [
        getattr(n, "name", None) for n in nodes]
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[])), str(PASSWORD_RESET), "exec"),
        namespace)
    return namespace


def _runtime(body=None, session=("account-bi", None), locked_until=0,
             session_id="session-current"):
    now = 5_000_000
    account_rec = {
        "name": "alice",
        "status": "active",
        "kind": "user",
        "email": "alice@example.com",
        "email_verified": True,
        "pass_salt": "salt-oldpass1",
        "pass_hash": "hash-oldpass1",
    }
    account_bi, override = session
    rec = None if override is False else {**account_rec, **(override or {})}
    calls = {
        "verify": [],
        "saved": [],
        "sql": [],
        "revoked_all": [],
        "cleared": [],
        "audits": [],
        "failures": [],
        "emails": [],
        "recorded": [],
    }

    def json_response(data, status=200, cache_seconds=None, cache_control=None,
                      extra_headers=None):
        return {"status": status, "body": data,
                "headers": dict(extra_headers or {}),
                "cacheControl": cache_control}

    async def bounded_json_request(request):
        return dict(request.body)

    async def account_session_record(_env, _request, _data=None,
                                     allow_unverified=False):
        return (account_bi, rec) if rec else ("", None)

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def login_locked_until(_env, _key):
        return locked_until

    async def verify_password(password, salt, digest):
        calls["verify"].append((password, salt, digest))
        return digest == "hash-" + str(password)

    async def hash_password(password):
        return ("salt-" + password, "hash-" + password)

    async def save_account(_env, name_bi, record):
        calls["saved"].append((name_bi, copy.deepcopy(record)))

    async def d1_run(_env, sql, *args):
        calls["sql"].append((sql, args))

    async def revoke_sessions(_env, name_bi, target=""):
        calls["revoked_all"].append((name_bi, target))

    async def login_clear(_env, key):
        calls["cleared"].append(key)

    async def audit(_env, actor, action, target_type="", target="",
                    outcome="success", details=None):
        calls["audits"].append((actor, action, outcome, dict(details or {})))

    async def login_failure_response(_env, keys, error):
        calls["failures"].append((list(keys), error))
        return {"status": 401, "body": {"error": error}, "headers": {},
                "cacheControl": None}

    async def send_email(_env, to_email, subject, _text, _html=None,
                         email_kind="account", **_kwargs):
        calls["emails"].append((to_email, subject, email_kind))
        return True

    def stamp_account_email(record, kind, ok, now=None):
        calls["recorded"].append((record.get("name"), kind, ok))
        record["last_email_kind"] = kind
        record["last_email_ok"] = bool(ok)
        return record

    namespace = {
        "Date": SimpleNamespace(now=lambda: now),
        "MAX_NODE_NAME": 40,
        "bounded_json_request": bounded_json_request,
        "blind_index": blind_index,
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "d1_run": d1_run,
        "hash_password": hash_password,
        "json_response": json_response,
        "verify_password": verify_password,
        "_account_revoke_sessions": revoke_sessions,
        "_account_session_record": account_session_record,
        "_audit_sensitive_action": audit,
        "_forkmesh_email_card_html": lambda *args, **kwargs: "<html></html>",
        "_html_escape": lambda value: str(value or ""),
        "_login_clear": login_clear,
        "_login_failure_response": login_failure_response,
        "_login_locked_until": login_locked_until,
        "_public_base_url": lambda _env, _request=None: "https://app.forkmesh.com",
        "_stamp_account_email": stamp_account_email,
        "_request_account_session_id": lambda _request, _data=None: session_id,
        "_save_account": save_account,
        "_send_email": send_email,
    }
    _load(namespace)
    request = SimpleNamespace(
        method="POST", headers={},
        url="https://app.forkmesh.com/api/accounts/change-password",
        body=body if body is not None else {
            "currentPassword": "oldpass1", "newPassword": "newpass12"})
    response = asyncio.run(
        namespace["_account_change_password"](object(), request))
    return response, calls, namespace


def test_change_password_rehashes_and_keeps_this_device_signed_in():
    response, calls, _ns = _runtime()
    assert response["status"] == 200
    assert response["body"] == {"ok": True, "currentRevoked": False}
    assert response["cacheControl"] == "no-store, max-age=0, must-revalidate"
    _, saved = calls["saved"][0]
    assert saved["pass_hash"] == "hash-newpass12"
    assert saved["pass_salt"] == "salt-newpass12"
    assert saved["password_changed_at"] == 5_000_000
    # Only the other devices go: one scoped UPDATE, never the revoke-everything
    # helper, so the settings screen the change was typed on stays usable.
    assert calls["revoked_all"] == []
    sql, args = calls["sql"][0]
    assert "UPDATE account_sessions SET revoked_at=?" in sql
    assert "session_id<>?" in sql
    assert args == (5_000_000, "account-bi", "session-current")
    assert calls["audits"] == [
        ("alice", "account.password_change", "success",
         {"currentRevoked": False})]


def test_an_unidentifiable_session_falls_back_to_signing_everything_out():
    # The handler should always know which session called it; if it somehow
    # cannot, it clears them all rather than leaving another device holding a
    # session minted against the old password, and says so to the caller.
    response, calls, _ns = _runtime(session_id="")
    assert response["status"] == 200
    assert response["body"] == {"ok": True, "currentRevoked": True}
    assert calls["revoked_all"] == [("account-bi", "")]
    assert not any("account_sessions" in sql for sql, _args in calls["sql"])
    assert calls["audits"] == [
        ("alice", "account.password_change", "success",
         {"currentRevoked": True})]


def test_a_wrong_current_password_changes_nothing_and_is_counted():
    response, calls, _ns = _runtime(body={
        "currentPassword": "not-the-password", "newPassword": "newpass12"})
    assert response["status"] == 401
    assert response["body"] == {"error": "invalid_credentials"}
    assert calls["saved"] == []
    assert calls["sql"] == []
    assert calls["revoked_all"] == []
    assert calls["failures"] == [
        (["bi:password-change-v1\naccount-bi"], "invalid_credentials")]
    assert calls["audits"] == [
        ("alice", "account.password_change", "denied",
         {"reason": "invalid_credentials"})]


def test_attempt_budget_is_separate_from_the_shared_login_budget():
    # A signed-in user fumbling their current password must not lock their own
    # address out of the login form, so the key has its own namespace.
    _response, _calls, ns = _runtime()
    key = asyncio.run(ns["_password_change_attempt_key"](object(), "account-bi"))
    assert key == "bi:password-change-v1\naccount-bi"


def test_a_locked_budget_refuses_before_spending_a_password_check():
    response, calls, _ns = _runtime(locked_until=5_030_000)
    assert response["status"] == 429
    assert response["body"] == {"error": "too_many_attempts",
                                "retryAfterMs": 30_000}
    assert response["headers"] == {"Retry-After": "30"}
    assert calls["verify"] == []
    assert calls["saved"] == []


def test_a_short_new_password_is_refused_before_any_credential_check():
    response, calls, ns = _runtime(body={
        "currentPassword": "oldpass1", "newPassword": "short"})
    assert response["status"] == 400
    assert response["body"] == {"error": "password_too_short"}
    assert ns["PASSWORD_CHANGE_MIN_LENGTH"] == 8
    assert calls["verify"] == []
    assert calls["saved"] == []


def test_reusing_the_current_password_is_refused():
    response, calls, _ns = _runtime(body={
        "currentPassword": "oldpass1", "newPassword": "oldpass1"})
    assert response["status"] == 400
    assert response["body"] == {"error": "password_unchanged"}
    assert calls["saved"] == []
    assert calls["sql"] == []


def test_a_key_bound_record_with_no_password_is_refused():
    response, calls, _ns = _runtime(
        session=("account-bi", {"pass_hash": "", "pass_salt": ""}))
    assert response["status"] == 400
    assert response["body"] == {"error": "password_not_set"}
    assert calls["verify"] == []
    assert calls["saved"] == []


def test_a_signed_out_or_disabled_caller_cannot_change_a_password():
    for session in (("", False), ("account-bi", {"status": "disabled"})):
        response, calls, _ns = _runtime(session=session)
        assert response["status"] == 401
        assert response["body"] == {"error": "invalid_session"}
        assert calls["saved"] == []
        assert calls["verify"] == []


def test_the_owner_is_emailed_that_the_password_changed():
    _response, calls, _ns = _runtime()
    assert calls["emails"] == [
        ("alice@example.com", "Your ForkMesh password was changed",
         "password_change")]
    assert calls["recorded"] == [("alice", "password_change", True)]
    # The send is stamped onto the record already in hand and saved again,
    # never re-read: a D1 replica still serving the pre-change row would put
    # the old password hash straight back.
    assert len(calls["saved"]) == 2
    assert calls["saved"][-1][1]["pass_hash"] == "hash-newpass12"
    assert calls["saved"][-1][1]["last_email_kind"] == "password_change"


def test_a_failed_notice_send_never_undoes_the_change():
    # The password is already replaced by the time the notice goes out; a mail
    # outage must not surface as a failed change the user then repeats.
    _response, _calls, ns = _runtime()

    async def exploding_send(*_args, **_kwargs):
        raise RuntimeError("mailtrap down")

    ns["_send_email"] = exploding_send
    sent = asyncio.run(ns["_send_password_changed_notice"](
        object(), SimpleNamespace(url=""), "alice", "alice@example.com"))
    assert sent is False


def test_an_account_with_no_address_on_file_is_not_emailed():
    _response, calls, _ns = _runtime(session=("account-bi", {"email": ""}))
    assert calls["emails"] == []
    assert calls["recorded"] == []
    assert len(calls["saved"]) == 1


def test_the_route_is_registered_for_post_only():
    assert (
        'if url.path == "/api/accounts/change-password" and method == "POST":'
        "\n        return await _account_change_password(env, request)"
    ) in ENTRY_TEXT
    assert (
        '_account_change_password = _password_reset.export(\n'
        '    "_account_change_password")'
    ) in ENTRY_TEXT


def test_the_notice_kind_is_a_known_email_category_but_pings_no_admin():
    # Categorized for the mail provider, deliberately absent from the ping
    # labels: a self-service password change needs no administrator follow-up.
    categories = ENTRY_TEXT[ENTRY_TEXT.index("MAILTRAP_EMAIL_CATEGORIES = {"):]
    assert '"password_change": "Password change",' in categories[:categories.index("}")]
    pings = ENTRY_TEXT[ENTRY_TEXT.index("ACCOUNT_EMAIL_PING_LABELS = {"):]
    assert "password_change" not in pings[:pings.index("}")]


def test_settings_exposes_a_change_password_form_that_posts_it():
    settings = (PUBLIC / "dashboard" / "partials" / "views" /
                "settings.html").read_text(encoding="utf-8")
    fragment = (PUBLIC / "dashboard" / "js" / "04-account.js").read_text(
        encoding="utf-8")
    for marker in ("data-account-password-current",
                   "data-account-password-new",
                   "data-account-password-confirm",
                   "data-account-password-save",
                   "data-account-password-status"):
        assert marker in settings, marker
        assert marker in fragment, marker
    assert ">Password<" in settings
    assert 'fetch("/api/accounts/change-password"' in fragment
    assert "bindAccountPasswordControls();" in fragment


def test_the_built_settings_document_ships_the_form():
    built = (PUBLIC / "dashboard" / "settings" / "index.html").read_text(
        encoding="utf-8")
    bundle = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")
    assert "data-account-password-save" in built
    assert 'fetch("/api/accounts/change-password"' in bundle

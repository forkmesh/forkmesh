#!/usr/bin/env python3
"""Self-service "verify again" for an unverified account email.

Contract under test, extracted straight from entry.py:
- POST /api/accounts/resend-verification authorizes on the signed-in session
  alone (the password-gated profile POST was the friction that left accounts
  unverified), and refuses a guest, a missing address, or an already-verified
  address;
- every send is recorded on the recipient's own record: a running total, the
  coarse kind, the outcome, and a short bounded history;
- that recorded history is the rate limit — a send the provider REJECTED counts
  exactly like a delivered one, so a broken provider cannot be hammered;
- a rejected send still falls back to the manual verification queue;
- administrators are pinged whenever transactional account mail goes out, and
  cron digest kinds are counted but deliberately never pinged.
"""

import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
TEXT = ENTRY.read_text(encoding="utf-8")

FUNCS = {
    "_stamp_account_email",
    "_account_email_sends",
    "_account_email_activity",
    "_record_account_email",
    "_notify_admins_account_email",
    "_account_resend_verification",
    "_verification_resend_refusal",
}
ASSIGNS = {
    "ACCOUNT_EMAIL_STATUS_DELIVERED",
    "ACCOUNT_EMAIL_STATUS_FAILED",
    "ACCOUNT_EMAIL_HISTORY",
    "ACCOUNT_EMAIL_PING_LABELS",
    "VERIFICATION_RESEND_COOLDOWN_MS",
    "VERIFICATION_RESEND_WINDOW_MS",
    "VERIFICATION_RESEND_WINDOW_LIMIT",
}

NOW = 2_000_000_000_000
MINUTE = 60 * 1000
DAY = 24 * 60 * 60 * 1000


def _load(extra):
    tree = ast.parse(TEXT, filename=str(ENTRY))
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
    assert FUNCS <= found | {""}
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _harness(rec=None, admins=(), sent=True, now=NOW):
    """A resend runtime over one signed-in account and an admin roster."""
    saved = {}
    queued = []
    pings = []
    sends = []
    claims = set()

    class _DateStub:
        @staticmethod
        def now():
            return now

    def json_response(body, status=200, **_kwargs):
        return {"status": status, "body": body}

    async def _account_session_record(_env, _request, _data=None,
                                      allow_unverified=False):
        # Being unverified is this endpoint's whole precondition, so it is one
        # of the few writes that opts out of the verification gate (#592).
        assert allow_unverified is True
        return ("bi:alice", rec) if rec else ("", None)

    async def _account_row(_env, name):
        return ("bi:" + name, dict(rec or {}))

    async def _save_account(_env, account_bi, record):
        saved[account_bi] = dict(record)

    async def _send_verification_email(env, _request, name, email):
        sends.append((name, email))
        await asyncio.sleep(0)
        await namespace["_record_account_email"](env, name, "verification", sent)
        return sent

    async def _enqueue_verification(_env, name_bi, name, email):
        queued.append((name_bi, name, email))

    async def d1_all(_env, sql, *_args):
        assert "is_admin=1" in sql
        return [{"data": {"name": admin}} for admin in admins]

    async def decrypt_row(_env, stored, key=None):
        return dict(stored) if isinstance(stored, dict) else None

    async def enqueue_notification(_env, recipient, kind, title, **kwargs):
        pings.append({"recipient": recipient, "kind": kind, "title": title,
                      **kwargs})
        return True

    async def bounded_json_request(_request):
        return {}

    async def blind_index(_env, value):
        return "bi:" + value

    async def d1_run(_env, sql, key, _created_at):
        assert "INSERT OR IGNORE INTO notification_email_sends" in sql
        changes = 0 if key in claims else 1
        claims.add(key)
        return SimpleNamespace(meta=SimpleNamespace(changes=changes))

    namespace = _load({
        "Date": _DateStub,
        "MAX_NODE_NAME": 63,
        "clean_string": lambda value, maximum=240: str(value or "")[:maximum],
        "valid_node_name": lambda name: bool(name) and name.isascii(),
        "json_response": json_response,
        "bounded_json_request": bounded_json_request,
        "_account_session_record": _account_session_record,
        "_account_row": _account_row,
        "_save_account": _save_account,
        "_send_verification_email": _send_verification_email,
        "_enqueue_verification": _enqueue_verification,
        "enqueue_notification": enqueue_notification,
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "blind_index": blind_index,
        "d1_run": d1_run,
    })
    namespace["_saved"] = saved
    namespace["_queued"] = queued
    namespace["_pings"] = pings
    namespace["_sends"] = sends
    namespace["_claims"] = claims
    return namespace


def _account(email="alice@example.com", verified=False, history=None,
             count=0):
    rec = {"name": "alice", "email": email, "email_verified": verified}
    if history is not None:
        rec["email_sends"] = history
        rec["email_send_count"] = count or len(history)
    return rec


def _resend(namespace):
    return asyncio.run(namespace["_account_resend_verification"](
        object(), SimpleNamespace(method="POST", headers={})))


# --- the button's endpoint --------------------------------------------------

def test_resend_needs_only_the_session_and_records_the_send():
    namespace = _harness(_account())
    response = _resend(namespace)
    assert response["status"] == 200
    assert response["body"]["verificationSent"] is True
    assert response["body"]["verificationQueued"] is False
    assert namespace["_sends"] == [("alice", "alice@example.com")]
    # The send is stamped on the recipient's own record: total, kind, outcome.
    stamped = namespace["_saved"]["bi:alice"]
    assert stamped["email_send_count"] == 1
    assert stamped["last_email_kind"] == "verification"
    assert stamped["last_email_ok"] is True
    assert stamped["email_sends"] == [
        {"ts": NOW, "kind": "verification", "ok": True}]
    assert response["body"]["emailSendCount"] == 1
    assert response["body"]["lastEmailStatus"] == "delivered"
    # No password was asked for anywhere on this path.
    assert "password" not in response["body"]


def test_resend_rejects_guests_missing_and_verified_addresses():
    assert _resend(_harness(None))["status"] == 401
    assert _resend(_harness(_account(email="")))["status"] == 400
    already = _resend(_harness(_account(verified=True)))
    assert already["status"] == 409
    assert already["body"]["error"] == "already_verified"


def test_rejected_send_is_queued_for_an_admin_and_still_counted():
    namespace = _harness(_account(), admins=("root",), sent=False)
    response = _resend(namespace)
    assert response["status"] == 200
    assert response["body"]["verificationQueued"] is True
    assert namespace["_queued"] == [
        ("bi:alice", "alice", "alice@example.com")]
    assert namespace["_saved"]["bi:alice"]["email_sends"] == [
        {"ts": NOW, "kind": "verification", "ok": False}]
    assert response["body"]["lastEmailStatus"] == "failed"
    # The administrator is told the provider rejected it, not that it landed.
    assert namespace["_pings"][0]["meta"]["delivered"] is False
    assert "rejected" in namespace["_pings"][0]["body"]


# --- the recorded history IS the rate limit ---------------------------------

def test_a_second_send_inside_the_cooldown_is_refused():
    namespace = _harness(_account(history=[
        {"ts": NOW - 5_000, "kind": "verification", "ok": True}]))
    response = _resend(namespace)
    assert response["status"] == 429
    assert response["body"]["error"] == "resend_too_soon"
    assert response["body"]["retryAfterMs"] == MINUTE - 5_000
    assert namespace["_sends"] == []


def test_parallel_resends_send_one_email():
    namespace = _harness(_account())

    async def burst():
        return await asyncio.gather(*[
            namespace["_account_resend_verification"](
                object(), SimpleNamespace(method="POST", headers={}))
            for _ in range(20)])

    responses = asyncio.run(burst())
    assert [r["status"] for r in responses].count(200) == 1
    assert {r["body"]["error"] for r in responses if r["status"] == 429} == {
        "resend_too_soon"}
    assert namespace["_sends"] == [("alice", "alice@example.com")]


def test_resend_is_refused_when_the_claim_cannot_be_written():
    namespace = _harness(_account())

    async def down(*_args):
        raise RuntimeError("d1 down")

    namespace["d1_run"] = down
    response = _resend(namespace)
    assert response["status"] == 429
    assert namespace["_sends"] == []


def test_failed_sends_count_against_the_daily_limit():
    # Five rejected sends still exhaust the window: a broken mail provider must
    # not turn the button into an unbounded send loop.
    namespace = _harness(_account(history=[
        {"ts": NOW - (index + 2) * MINUTE, "kind": "verification", "ok": False}
        for index in range(5)]))
    response = _resend(namespace)
    assert response["status"] == 429
    assert response["body"]["error"] == "resend_limit_reached"
    assert response["body"]["retryAfterMs"] > 0
    assert namespace["_sends"] == []


def test_sends_outside_the_window_and_other_kinds_do_not_block():
    namespace = _harness(_account(history=[
        {"ts": NOW - DAY - MINUTE, "kind": "verification", "ok": True},
        {"ts": NOW - 2 * MINUTE, "kind": "notifications", "ok": True},
        {"ts": NOW - 3 * MINUTE, "kind": "password_reset", "ok": True},
    ]))
    response = _resend(namespace)
    assert response["status"] == 200
    assert namespace["_sends"] == [("alice", "alice@example.com")]
    assert response["body"]["remaining"] == 4


def test_history_stays_bounded_and_the_total_keeps_climbing():
    namespace = _harness(_account())
    stamp = namespace["_stamp_account_email"]
    rec = {}
    for index in range(namespace["ACCOUNT_EMAIL_HISTORY"] + 6):
        stamp(rec, "notifications", True, NOW + index)
    assert len(rec["email_sends"]) == namespace["ACCOUNT_EMAIL_HISTORY"]
    assert rec["email_send_count"] == namespace["ACCOUNT_EMAIL_HISTORY"] + 6
    assert rec["email_sends"][0]["ts"] == NOW + 6
    # Neither subject nor body nor recipient address is ever kept.
    assert set(rec["email_sends"][0]) == {"ts", "kind", "ok"}


def test_activity_floors_the_counter_for_records_predating_it():
    namespace = _harness(_account())
    activity = namespace["_account_email_activity"](
        {"last_email_ts": NOW, "last_email_kind": "verification",
         "last_email_ok": True})
    assert activity["emailSendCount"] == 1
    assert namespace["_account_email_activity"]({})["emailSendCount"] == 0


# --- administrator pings ----------------------------------------------------

def test_every_admin_is_pinged_when_transactional_mail_goes_out():
    namespace = _harness(_account(), admins=("root", "ops"))
    _resend(namespace)
    assert [ping["recipient"] for ping in namespace["_pings"]] == [
        "root", "ops"]
    ping = namespace["_pings"][0]
    assert ping["kind"] == "account_email_sent"
    assert ping["title"] == "Verification email sent to alice"
    assert ping["actor"] == "alice"
    assert ping["meta"] == {"emailKind": "verification", "delivered": True,
                            "account": "alice"}
    assert ping["dedupe"] == "account-email:alice:verification:%d" % NOW


def test_password_resets_ping_but_cron_digests_are_only_counted():
    namespace = _harness(_account(), admins=("root",))
    notify = namespace["_notify_admins_account_email"]
    asyncio.run(notify(object(), "alice", "password_reset", True))
    assert namespace["_pings"][0]["title"] == "Password reset email sent to alice"
    # Digest mail runs over every account on a cron tick. Pinging per digest
    # would fan out into a notification storm, so those kinds are counted on the
    # recipient's record and stop there.
    for kind in ("notifications", "general_chat", "feedback", ""):
        asyncio.run(notify(object(), "alice", kind, True))
    assert len(namespace["_pings"]) == 1


def test_admin_ping_failure_never_breaks_the_send():
    namespace = _harness(_account(), admins=("root",))

    async def boom(*_args, **_kwargs):
        raise RuntimeError("d1 down")

    namespace["d1_all"] = boom
    namespace["_pings"].clear()
    asyncio.run(namespace["_notify_admins_account_email"](
        object(), "alice", "verification", True))
    assert namespace["_pings"] == []


# --- wiring -----------------------------------------------------------------

def test_route_and_notification_kind_are_registered():
    assert ('if url.path == "/api/accounts/resend-verification" '
            'and method == "POST":') in TEXT
    assert "return await _account_resend_verification(env, request)" in TEXT


def test_profile_resend_goes_through_the_same_limit():
    start = TEXT.index('if data.get("resendVerification")')
    block = TEXT[start:TEXT.index("new_name = clean_string", start)]
    assert block.index("_verification_resend_refusal(") < block.index(
        "_send_verification_email(")
    assert "if verification_refusal is None:" in block
    assert 'payload["verificationError"]' in TEXT
    assert '"account_email_sent",' in TEXT


def test_dashboard_shows_a_resend_button_while_the_email_is_unverified():
    banner = (ROOT / "public" / "dashboard" / "partials" / "header.html"
              ).read_text(encoding="utf-8")
    assert "data-email-verification-banner" in banner
    assert "data-email-verification-resend" in banner
    for page in ("index.html", "settings/index.html", "repo.html"):
        built = (ROOT / "public" / "dashboard" / page).read_text(
            encoding="utf-8")
        assert "data-email-verification-resend" in built, page
    bundle = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
    assert "renderEmailVerificationBanner" in bundle
    assert '"/api/accounts/resend-verification"' in bundle

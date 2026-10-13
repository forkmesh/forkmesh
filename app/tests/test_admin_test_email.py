#!/usr/bin/env python3

import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
TEXT = ENTRY.read_text(encoding="utf-8")
PUBLIC = ROOT / "public"

FUNCS = {
    "_stamp_account_email",
    "_account_email_sends",
    "_account_email_activity",
    "_account_admin_test_email",
}
ASSIGNS = {
    "ACCOUNT_EMAIL_STATUS_DELIVERED",
    "ACCOUNT_EMAIL_STATUS_FAILED",
    "ACCOUNT_EMAIL_HISTORY",
    "ADMIN_TEST_EMAIL_COOLDOWN_MS",
}

NOW = 2_000_000_000_000


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
    assert FUNCS <= {getattr(n, "name", "") for n in selected}
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _harness(rec, admin=True, sent=True, configured=True):
    sends = []
    recorded = []

    class _DateStub:
        @staticmethod
        def now():
            return NOW

    async def _account_session_record(_env, _request, _data=None):
        return ("bi:alice", rec) if rec else ("", None)

    async def _is_admin(_env, name):
        return admin and name == "alice"

    async def _send_email(_env, to_email, subject, text, html=None,
                          email_kind="account"):
        sends.append({"to": to_email, "subject": subject, "kind": email_kind,
                      "text": text, "html": html})
        return sent

    async def _record_account_email(_env, name, kind, ok):
        recorded.append((name, kind, ok))

    async def bounded_json_request(_request):
        return {}

    namespace = _load({
        "Date": _DateStub,
        "MAX_NODE_NAME": 63,
        "clean_string": lambda value, maximum=240: str(value or "")[:maximum],
        "valid_node_name": lambda name: bool(name) and name.isascii(),
        "json_response": lambda body, status=200, **_k: {
            "status": status, "body": body},
        "bounded_json_request": bounded_json_request,
        "_account_session_record": _account_session_record,
        "_is_admin": _is_admin,
        "_send_email": _send_email,
        "_record_account_email": _record_account_email,
        "_html_escape": lambda value: str(value),
        "_forkmesh_email_card_html": lambda h, i, b, *a, **k: h + i + b,
        "smtp_mail": SimpleNamespace(enabled=lambda _env: False),
    })
    env = SimpleNamespace(MAILTRAP_API_TOKEN="token" if configured else "")
    namespace["_env"] = env
    namespace["_sends"] = sends
    namespace["_recorded"] = recorded
    return namespace


def _call(namespace):
    return asyncio.run(namespace["_account_admin_test_email"](
        namespace["_env"], SimpleNamespace(method="POST", headers={})))


def _account(email="alice@example.com", history=None):
    rec = {"name": "alice", "email": email}
    if history is not None:
        rec["email_sends"] = history
        rec["email_send_count"] = len(history)
    return rec


def test_admin_gets_a_test_email_at_their_own_address():
    namespace = _harness(_account())
    response = _call(namespace)
    assert response["status"] == 200
    assert response["body"]["sent"] is True
    assert response["body"]["email"] == "alice@example.com"
    assert namespace["_sends"][0]["to"] == "alice@example.com"
    assert namespace["_sends"][0]["kind"] == "admin_test"
    assert namespace["_recorded"] == [("alice", "admin_test", True)]
    assert response["body"]["lastEmailKind"] == "admin_test"
    assert response["body"]["lastEmailStatus"] == "delivered"


def test_non_admins_guests_and_missing_addresses_are_refused():
    assert _call(_harness(None))["status"] == 401
    refused = _harness(_account(), admin=False)
    assert _call(refused)["status"] == 403
    assert refused["_sends"] == []
    assert _call(_harness(_account(email="")))["status"] == 400


def test_one_test_email_per_minute():
    recent = [{"ts": NOW - 10_000, "kind": "admin_test", "ok": True}]
    namespace = _harness(_account(history=recent))
    response = _call(namespace)
    assert response["status"] == 429
    assert response["body"]["retryAfterMs"] == 50_000
    assert namespace["_sends"] == []
    older = [{"ts": NOW - 61_000, "kind": "admin_test", "ok": True}]
    assert _call(_harness(_account(history=older)))["status"] == 200


def test_unconfigured_mail_reports_it_without_sending():
    namespace = _harness(_account(), configured=False)
    response = _call(namespace)
    assert response["body"]["sent"] is False
    assert response["body"]["configured"] is False
    assert namespace["_sends"] == []
    assert namespace["_recorded"] == [("alice", "admin_test", False)]


def test_route_and_settings_button_are_wired():
    assert ('url.path == "/api/accounts/test-email" and method == "POST"'
            in TEXT)
    view = (PUBLIC / "dashboard/partials/views/settings.html").read_text(
        encoding="utf-8")
    assert "data-admin-test-email-row" in view
    assert "data-admin-test-email " in view or "data-admin-test-email\n" in view
    account = (PUBLIC / "dashboard/js/04-account.js").read_text(
        encoding="utf-8")
    assert '"/api/accounts/test-email"' in account
    assert '$("[data-admin-test-email-row]")?.classList.toggle("hidden", session?.isAdmin !== true)' in account
    network = (PUBLIC / "dashboard/js/08-repo-detail-network.js").read_text(
        encoding="utf-8")
    assert 'closest("[data-admin-test-email]")' in network

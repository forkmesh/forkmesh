#!/usr/bin/env python3

import ast
import asyncio
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
SIGNUP_JS = (ROOT / "public" / "signup.js").read_text(encoding="utf-8")

FUNCS = {
    "ping_signup_failure",
    "_signup_failure_already_pinged",
    "_signup_refused",
    "_account_signup_problem",
}
CONSTS = {
    "SIGNUP_PROBLEM_PATH",
    "SIGNUP_BROWSER_PROBLEMS",
    "SIGNUP_FAILURE_WINDOW_MS",
    "SIGNUP_FAILURE_PINGS_PER_WINDOW",
    "_signup_failure_pinged",
}


class _Date:
    value = 1700000000000

    @classmethod
    def now(cls):
        return cls.value


class _Response:
    def __init__(self, body, status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = headers or {}


def _load(admins=("root", "alice"), claim=None, origin_ok=True, payload=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in FUNCS)
        or (isinstance(node, ast.Assign) and {
            target.id for target in node.targets
            if isinstance(target, ast.Name)} & CONSTS)
    ]
    sent = []

    async def d1_all(_env, sql, *params):
        assert "is_admin=1" in sql
        return [{"data": name} for name in admins]

    async def decrypt_row(_env, data):
        return {"name": data}

    async def enqueue_notification(env, recipient, kind, title, **kwargs):
        sent.append({"recipient": recipient, "kind": kind, "title": title,
                     **kwargs})
        return True

    async def bounded_json_request(_request):
        return payload

    def json_response(data, status=200, cache_control=None,
                      extra_headers=None):
        return _Response(json.dumps(data), status=status)

    namespace = {
        "re": re,
        "Date": _Date,
        "MAX_NODE_NAME": 63,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?", value or "")),
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "enqueue_notification": enqueue_notification,
        "bounded_json_request": bounded_json_request,
        "json_response": json_response,
        "_request_origin_allowed": lambda env, request: origin_ok,
        "_account_signup_intent_claim": lambda env, request: claim,
    }
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace, sent


def test_every_server_refusal_pings_every_administrator():
    ns, sent = _load()
    response = asyncio.run(ns["_signup_refused"](
        object(), "node_name_taken", 409, claim={"claimId": "c1"},
        name="solvingsoft"))
    assert response.status == 409
    assert json.loads(response.body) == {"error": "node_name_taken"}
    assert [item["recipient"] for item in sent] == ["root", "alice"]
    assert {item["kind"] for item in sent} == {"operational_alert"}
    assert "node_name_taken" in sent[0]["title"]
    assert "@solvingsoft" in sent[0]["body"]
    assert "409" in sent[0]["body"]


def test_a_supplied_response_is_returned_and_its_status_reported():
    ns, sent = _load(admins=("root",))
    page = _Response("{}", status=403)
    result = asyncio.run(ns["_signup_refused"](
        object(), "turnstile_expired", 400, claim={"claimId": "c2"},
        response=page))
    assert result is page
    assert "HTTP 403" in sent[0]["body"]


def test_a_turnstile_refusal_names_the_siteverify_detail():
    ns, sent = _load(admins=("root",))
    asyncio.run(ns["_signup_refused"](
        object(), "turnstile_failed", 403, claim={"claimId": "c9"},
        detail="siteverify:invalid-input-response"))
    assert sent[0]["body"].endswith("[siteverify:invalid-input-response]")
    handler = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_signup(env, request):"):
        ENTRY_TEXT.index("async def _account_password_record")]
    assert 'detail=turnstile_outcome.get("detail", "")' in handler
    gate = ENTRY_TEXT[ENTRY_TEXT.index("async def _turnstile_gate("):]
    assert 'outcome["detail"] = detail' in gate[:gate.index("\nasync def ")]


def test_every_turnstile_refusal_has_its_own_page_message():
    turnstile = (ROOT / "src" / "turnstile.py").read_text(encoding="utf-8")
    codes = set(re.findall(r'^[A-Z_]+ = "(turnstile_[a-z_]+)"', turnstile,
                           re.M))
    assert "turnstile_stale_page" in codes
    for code in codes:
        assert 'body.error === "%s"' % code in SIGNUP_JS


def test_the_ping_never_carries_an_email_address():
    ns, sent = _load(admins=("root",))
    asyncio.run(ns["ping_signup_failure"](
        object(), "valid_email_required", 400, claim={"claimId": "c3"},
        name="someone@example.com", detail="someone@example.com"))
    text = sent[0]["title"] + sent[0]["body"] + json.dumps(sent[0]["meta"])
    assert "@example.com" not in text
    assert "a visitor" in sent[0]["body"]


def test_a_repeat_from_the_same_page_view_pings_once():
    ns, sent = _load(admins=("root",))
    for _ in range(3):
        asyncio.run(ns["ping_signup_failure"](
            object(), "email_taken", 409, claim={"claimId": "c4"}))
    asyncio.run(ns["ping_signup_failure"](
        object(), "email_taken", 409, claim={"claimId": "c5"}))
    assert len(sent) == 2


def test_requests_without_a_page_view_ping_once_per_window_per_reason():
    ns, sent = _load(admins=("root",))
    for _ in range(5):
        asyncio.run(ns["ping_signup_failure"](
            object(), "signup_page_required", 403))
    assert len(sent) == 1
    _Date.value += ns["SIGNUP_FAILURE_WINDOW_MS"]
    try:
        asyncio.run(ns["ping_signup_failure"](
            object(), "signup_page_required", 403))
    finally:
        _Date.value -= ns["SIGNUP_FAILURE_WINDOW_MS"]
    assert len(sent) == 2


def test_a_flood_is_capped_per_window():
    ns, sent = _load(admins=("root",))
    cap = ns["SIGNUP_FAILURE_PINGS_PER_WINDOW"]
    for index in range(cap + 10):
        asyncio.run(ns["ping_signup_failure"](
            object(), "turnstile_required", 403,
            claim={"claimId": "flood-%d" % index}))
    assert len(sent) == cap


def test_a_failing_ping_never_breaks_the_refusal():
    ns, _sent = _load()

    async def broken(*_args, **_kwargs):
        raise RuntimeError("database down")

    ns["d1_all"] = broken
    response = asyncio.run(ns["_signup_refused"](
        object(), "rate_limited", 429, claim={"claimId": "c6"}))
    assert response.status == 429


def test_the_browser_reports_a_missing_challenge_token():
    ns, sent = _load(
        admins=("root",), claim={"claimId": "c7"},
        payload={"problem": "challenge_token_missing",
                 "nodeName": "solvingsoft", "detail": ""})
    response = asyncio.run(ns["_account_signup_problem"](object(), object()))
    assert response.status == 202
    assert sent[0]["title"] == "Signup failed: challenge_token_missing"
    assert "browser" in sent[0]["body"]
    assert sent[0]["meta"]["source"] == "browser"


def test_browser_reports_need_a_signup_page_view_and_a_known_problem():
    for kwargs, status in (
            ({"claim": None, "payload": {"problem": "network_error"}}, 403),
            ({"claim": {"claimId": "c8"}, "origin_ok": False,
              "payload": {"problem": "network_error"}}, 403),
            ({"claim": {"claimId": "c9"},
              "payload": {"problem": "anything at all"}}, 400)):
        ns, sent = _load(admins=("root",), **kwargs)
        response = asyncio.run(ns["_account_signup_problem"](
            object(), object()))
        assert response.status == status
        assert sent == []


def test_every_problem_the_page_reports_is_accepted_by_the_server():
    ns, _sent = _load()
    reported = set(re.findall(r'reportProblem\("([a-z_]+)"', SIGNUP_JS))
    assert reported
    assert reported <= ns["SIGNUP_BROWSER_PROBLEMS"]
    assert '"/api/accounts/signup/problem"' in SIGNUP_JS
    assert ns["SIGNUP_PROBLEM_PATH"] == "/api/accounts/signup/problem"


def test_the_signup_handler_has_no_silent_refusal():
    handler = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_signup(env, request):"):
        ENTRY_TEXT.index("async def _account_password_record")]
    assert 'json_response({"error"' not in handler
    assert "return _signup_page_required_response()" not in handler
    assert "return refused\n" not in handler
    assert "return throttled\n" not in handler
    assert handler.count("await _signup_refused(") >= 15
    dispatch = ENTRY_TEXT[ENTRY_TEXT.index("async def accounts_handler"):]
    dispatch = dispatch[:dispatch.index("/api/accounts/reserve")]
    assert "ping_signup_failure(" in dispatch
    assert "_account_signup_problem(env, request)" in dispatch

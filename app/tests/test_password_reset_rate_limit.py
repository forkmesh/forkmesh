"""Anonymous password-reset requests have atomic, privacy-preserving limits."""

import ast
import asyncio
import ipaddress
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
TEXT = ENTRY.read_text(encoding="utf-8")
PASSWORD_RESET = ENTRY.with_name("password_reset.py")
PASSWORD_RESET_TEXT = PASSWORD_RESET.read_text(encoding="utf-8")
FUNCTIONS = {
    "_password_reset_requester_bucket",
    "_password_reset_requester_allowed",
    "_password_reset_claim_account",
}
V6_PREFIX = 64
ENDPOINT_FUNCTIONS = FUNCTIONS | {"_account_forgot_password"}


def _load(extra_globals, names=FUNCTIONS):
    tree = ast.parse(PASSWORD_RESET_TEXT, filename=str(PASSWORD_RESET))
    nodes = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in nodes} == set(names)
    namespace = dict(extra_globals)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[])), str(PASSWORD_RESET), "exec"),
         namespace)
    return namespace


def _harness():
    now = [1_000_000]
    rows = {}
    requester_window = 1_000
    account_window = 10_000
    cooldown = 100
    requester_limit = 3
    account_limit = 2

    class Date:
        @staticmethod
        def now():
            return now[0]

    async def d1_first(_env, sql, *args):
        key = args[0] if "INSERT" in sql else args[6]
        row = rows.get(key)
        if sql.startswith("INSERT INTO password_reset_rate"):
            if not row or now[0] - row["window_start_ts"] >= requester_window:
                row = {"count": 1, "window_start_ts": now[0],
                       "last_sent_ts": 0}
            else:
                row["count"] += 1
            rows[key] = row
            return dict(row)
        if sql.startswith("INSERT OR IGNORE"):
            if row:
                return None
            rows[key] = {"count": 1, "window_start_ts": now[0],
                         "last_sent_ts": now[0]}
            return {"key_bi": key}
        if sql.startswith("UPDATE password_reset_rate"):
            elapsed = now[0] - row["window_start_ts"]
            if (now[0] - row["last_sent_ts"] < cooldown
                    or (elapsed < account_window
                        and row["count"] >= account_limit)):
                return None
            if elapsed >= account_window:
                row["count"] = 1
                row["window_start_ts"] = now[0]
            else:
                row["count"] += 1
            row["last_sent_ts"] = now[0]
            return {"key_bi": key}
        raise AssertionError(sql)

    namespace = _load({
        "Date": Date,
        "d1_first": d1_first,
        "PASSWORD_RESET_REQUESTER_WINDOW_MS": requester_window,
        "PASSWORD_RESET_REQUESTER_WINDOW_LIMIT": requester_limit,
        "PASSWORD_RESET_ACCOUNT_WINDOW_MS": account_window,
        "PASSWORD_RESET_ACCOUNT_WINDOW_LIMIT": account_limit,
        "PASSWORD_RESET_ACCOUNT_COOLDOWN_MS": cooldown,
        "PASSWORD_RESET_REQUESTER_V6_PREFIX": V6_PREFIX,
        "ipaddress": ipaddress,
    })
    return namespace, now, rows


def test_requester_budget_is_independent_and_resets():
    ns, now, _rows = _harness()
    allowed = ns["_password_reset_requester_allowed"]
    for _ in range(3):
        assert asyncio.run(allowed(None, "requester:a"))
    assert not asyncio.run(allowed(None, "requester:a"))
    assert asyncio.run(allowed(None, "requester:b"))
    now[0] += 1_000
    assert asyncio.run(allowed(None, "requester:a"))


def test_account_claim_enforces_cooldown_and_daily_budget():
    ns, now, _rows = _harness()
    claim = ns["_password_reset_claim_account"]
    assert asyncio.run(claim(None, "account:a"))
    assert not asyncio.run(claim(None, "account:a"))
    now[0] += 100
    assert asyncio.run(claim(None, "account:a"))
    now[0] += 100
    assert not asyncio.run(claim(None, "account:a"))
    assert asyncio.run(claim(None, "account:b"))
    now[0] += 10_000
    assert asyncio.run(claim(None, "account:a"))


def test_requester_bucket_charges_ipv6_per_allocation():
    ns, _now, _rows = _harness()
    bucket = ns["_password_reset_requester_bucket"]
    # An IPv4 caller still pays as itself, so one NAT shares one budget.
    assert bucket("203.0.113.7") == "203.0.113.7"
    # Every address in one routed /64 is one customer, so all of them collapse
    # onto a single key an attacker cannot walk away from.
    assert bucket("2001:db8:0:1::1") == "2001:db8:0:1::/64"
    assert bucket("2001:db8:0:1:ffff:ffff:ffff:1") == "2001:db8:0:1::/64"
    assert bucket("2001:db8:0:2::1") != bucket("2001:db8:0:1::1")
    # The rendered prefix keeps the families apart even when an IPv4 string
    # would otherwise be a legal key on its own.
    assert bucket("2001:db8:0:1::/64") == ""
    assert bucket("") == ""
    assert bucket("not-an-address") == ""
    assert bucket(None) == ""


def test_ipv6_address_rotation_cannot_mint_fresh_requester_budgets():
    ns, _now, rows = _harness()
    bucket = ns["_password_reset_requester_bucket"]
    allowed = ns["_password_reset_requester_allowed"]

    def request_from(address):
        key = bucket(address)
        return asyncio.run(allowed(None, "bi:" + key if key else ""))

    # A fresh source address per request is free on IPv6; the budget must still
    # run out after requester_limit sends from that one allocation.
    for index in range(3):
        assert request_from("2001:db8:0:1::%d" % (index + 1))
    assert not request_from("2001:db8:0:1::dead")
    # A genuinely different allocation is still a different customer.
    assert request_from("2001:db8:0:2::1")
    assert len(rows) == 2


def test_forgot_password_keeps_rate_limit_outcome_enumeration_safe():
    start = PASSWORD_RESET_TEXT.index("async def _account_forgot_password")
    body = PASSWORD_RESET_TEXT[start:]
    assert "_password_reset_requester_bucket" in body
    assert "_password_reset_requester_allowed" in body
    assert "_password_reset_claim_account" in body
    assert body.count('{"ok": True}') >= 2
    assert "rate_limited" not in body


def test_requester_prefix_constant_ships_with_the_worker():
    # The helper resolves the width from entry.py's globals at bind time, so a
    # missing constant would only surface as a NameError on a live request.
    assert "PASSWORD_RESET_REQUESTER_V6_PREFIX = 64" in TEXT


def _endpoint_harness(client_ip="203.0.113.7", requester_limit=100,
                      account_limit=2):
    """Drive the real handler over stubbed Worker/D1 primitives.

    This is the issue's reproduction, not a unit test: one registered address
    submitted over and over from a browser, asserting on the mail that leaves
    rather than on which branch the handler took.
    """
    now = [1_000_000]
    rows = {}
    sent = []
    ip = [client_ip]
    account_window = 10_000
    cooldown = 100
    account = {
        "name": "victim",
        "status": "active",
        "email": "victim@example.com",
        "pass_hash": "stored-hash",
    }

    class Date:
        @staticmethod
        def now():
            return now[0]

    async def blind_index(_env, value):
        # Stands in for the keyed one-way index: distinct inputs stay distinct,
        # so a namespace collision would surface as a shared budget here.
        return "bi(" + str(value) + ")"

    async def d1_first(_env, sql, *args):
        if sql.startswith("SELECT user_bi,data FROM users"):
            if args[0] == "bi(" + account["email"] + ")":
                return {"user_bi": "bi(victim)", "data": "enc"}
            return None
        key = args[0] if sql.startswith("INSERT") else args[6]
        row = rows.get(key)
        if sql.startswith("INSERT INTO password_reset_rate"):
            if not row or now[0] - row["window_start_ts"] >= 1_000_000:
                row = {"count": 1, "window_start_ts": now[0],
                       "last_sent_ts": 0}
            else:
                row["count"] = min(row["count"] + 1, requester_limit + 1)
            rows[key] = row
            return dict(row)
        if sql.startswith("INSERT OR IGNORE"):
            if row:
                return None
            rows[key] = {"count": 1, "window_start_ts": now[0],
                         "last_sent_ts": now[0]}
            return {"key_bi": key}
        if sql.startswith("UPDATE password_reset_rate"):
            elapsed = now[0] - row["window_start_ts"]
            if (now[0] - row["last_sent_ts"] < cooldown
                    or (elapsed < account_window
                        and row["count"] >= account_limit)):
                return None
            if elapsed >= account_window:
                row["count"] = 1
                row["window_start_ts"] = now[0]
            else:
                row["count"] += 1
            row["last_sent_ts"] = now[0]
            return {"key_bi": key}
        raise AssertionError(sql)

    async def decrypt_row(_env, _data):
        return dict(account)

    async def account_row(_env, name):
        if name == account["name"]:
            return "bi(victim)", dict(account)
        return "", None

    async def send_reset(_env, _request, name, email, pass_hash):
        sent.append((name, email, pass_hash))
        return True

    async def turnstile_gate(_env, _request, _data, _action, _url, **_kw):
        return None  # the Turnstile gate has its own tests (test_turnstile.py)

    namespace = _load({
        "Date": Date,
        "d1_first": d1_first,
        "blind_index": blind_index,
        "decrypt_row": decrypt_row,
        "_turnstile_gate": turnstile_gate,
        "_turnstile": type("T", (), {"ACTION_PASSWORD_RESET": "password-reset"}),
        "_account_row": account_row,
        "_send_password_reset_email": send_reset,
        "_account_session_client_ip": lambda _request: ip[0],
        "bounded_json_request": lambda request: request,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: value.isalnum() or "-" in value,
        "json_response": lambda payload, **kwargs: dict(payload, **kwargs),
        "PASSWORD_RESET_REQUESTER_WINDOW_MS": 1_000_000,
        "PASSWORD_RESET_REQUESTER_WINDOW_LIMIT": requester_limit,
        "PASSWORD_RESET_ACCOUNT_WINDOW_MS": account_window,
        "PASSWORD_RESET_ACCOUNT_WINDOW_LIMIT": account_limit,
        "PASSWORD_RESET_ACCOUNT_COOLDOWN_MS": cooldown,
        "PASSWORD_RESET_REQUESTER_V6_PREFIX": V6_PREFIX,
        "ipaddress": ipaddress,
    }, names=ENDPOINT_FUNCTIONS)
    handler = namespace["_account_forgot_password"]

    def post(identifier):
        async def body():
            return {"identifier": identifier}
        return asyncio.run(handler(None, body()))

    return post, sent, now, ip, cooldown


def test_repeated_requests_for_one_address_send_one_email():
    # The report: the same registered address submitted ten times in a row
    # delivered ten reset messages.
    post, sent, _now, _ip, _cooldown = _endpoint_harness()
    responses = [post("victim@example.com") for _ in range(10)]
    assert len(sent) == 1
    # Suppression is invisible from outside: every answer is the same generic
    # body, so the endpoint still cannot be used to enumerate accounts.
    assert all(response["ok"] is True for response in responses)
    assert all("error" not in response for response in responses)


def test_email_and_node_name_share_one_account_budget():
    # Both lookup paths resolve to the same account key, so alternating between
    # them must not hand the sender a second cooldown to spend.
    post, sent, now, _ip, _cooldown = _endpoint_harness()
    post("victim@example.com")
    post("victim")
    assert len(sent) == 1
    now[0] += 100
    post("victim")
    assert len(sent) == 2


def test_unregistered_address_answers_the_same_and_sends_nothing():
    post, sent, _now, _ip, _cooldown = _endpoint_harness()
    assert post("stranger@example.com")["ok"] is True
    assert post("stranger")["ok"] is True
    assert sent == []


def test_ipv6_rotation_cannot_outrun_the_requester_budget():
    # A /64 costs an attacker one allocation, so rotating the source address
    # inside it must keep spending the same budget. The account cooldown is
    # stepped over and its window budget lifted, leaving the requester limit as
    # the only control that can hold the send back.
    post, sent, now, ip, cooldown = _endpoint_harness(
        client_ip="2001:db8:0:1::1", requester_limit=3, account_limit=1_000)

    def post_from(address):
        ip[0] = address
        now[0] += cooldown
        return post("victim@example.com")

    responses = [post_from("2001:db8:0:1::%d" % index) for index in range(6)]
    assert len(sent) == 3
    # A genuinely different allocation is a different customer and still gets
    # its own budget.
    assert post_from("2001:db8:0:2::1")["ok"] is True
    assert len(sent) == 4
    assert all(response["ok"] is True for response in responses)

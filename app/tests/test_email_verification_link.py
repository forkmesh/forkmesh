#!/usr/bin/env python3
import ast
import asyncio
import base64
import hashlib
import hmac
import re
from pathlib import Path
from urllib.parse import parse_qs, urlencode, urlparse

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
TEXT = ENTRY.read_text(encoding="utf-8")

FUNCS = {
    "_email_verification_issued_at",
    "_email_verify_link_token",
    "_email_verify_link_parts",
    "_email_verify_masked",
    "_email_verify_token",
    "_email_verify_link",
    "_email_verify_link_valid",
    "_verify_email",
}
ASSIGNS = {
    "EMAIL_VERIFICATION_TOKEN_TTL_MS",
    "EMAIL_VERIFICATION_FUTURE_SKEW_MS",
    "EMAIL_VERIFICATION_TTL_HOURS",
    "EMAIL_VERIFY_LINK_TOKEN_RE",
}
NOW = 1_700_000_000_000
KEY = b"test-data-key"


class _Date:
    now_ms = NOW

    @classmethod
    def now(cls):
        return cls.now_ms


class _Request:
    def __init__(self, method, url, body=""):
        self.method = method
        self.url = url
        self.body = body

    async def text(self):
        return self.body


def _load():
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
    store = {}
    events = []

    async def _email_verify_mac(_env, prefix, name, email, issued_at):
        msg = "%s\n%s\n%s\n%d" % (prefix, name, email, int(issued_at))
        return hmac.new(KEY, msg.encode(), hashlib.sha256).digest()

    async def _account_row(_env, name):
        rec = store.get(name)
        return ("bi:" + name, dict(rec)) if rec else ("", None)

    async def _save_account(_env, name_bi, rec):
        store[name_bi[3:]] = dict(rec)

    async def _finalize_referral_signup(_env, name_bi):
        events.append(("referral", name_bi))

    async def d1_run(_env, sql, *args):
        events.append((sql, args))

    class _Hackathon:
        @staticmethod
        async def on_email_verified(_env, name_bi, _name):
            events.append(("hackathon", name_bi))

    namespace = {
        "Date": _Date,
        "re": re,
        "hmac": hmac,
        "base64": base64,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "MAX_NODE_NAME": 63,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "method_name": lambda request: request.method,
        "_html_escape": lambda value: str(value).replace("&", "&amp;").replace(
            "<", "&lt;").replace('"', "&quot;"),
        "_verify_email_page": lambda message, status: {
            "status": status, "html": message},
        "_email_verify_mac": _email_verify_mac,
        "_account_row": _account_row,
        "_save_account": _save_account,
        "_finalize_referral_signup": _finalize_referral_signup,
        "_hackathon": _Hackathon,
        "d1_run": d1_run,
    }
    module = ast.fix_missing_locations(ast.Module(body=selected,
                                                  type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace, store, events


def _link(ns, name, email, issued_at):
    return asyncio.run(ns["_email_verify_link"](None, name, email, issued_at))


def _call(ns, method, params):
    query = urlencode(params)
    if method == "GET":
        request = _Request("GET", "https://forkmesh.com/api/accounts/"
                           "verify-email?" + query)
    else:
        request = _Request("POST", "https://forkmesh.com/api/accounts/"
                           "verify-email", query)
    return asyncio.run(ns["_verify_email"](None, request))


def _setup(**extra):
    ns, store, events = _load()
    _Date.now_ms = NOW
    store["alice"] = {"name": "alice", "email": "alice@example.com", **extra}
    store["bob"] = {"name": "bob", "email": "bob@example.com"}
    return ns, store, events


def test_get_shows_the_masked_address_and_post_confirms_once():
    ns, store, events = _setup(email_verify_issued_at=NOW)
    token = _link(ns, "alice", "alice@example.com", NOW)
    assert len(token) == 51 and str(NOW) not in token

    page = _call(ns, "GET", {"node": "alice", "token": token})
    assert page["status"] == 200
    assert "a•••@example.com" in page["html"]
    assert "alice@example.com" not in page["html"]
    assert 'name="issuedAt"' not in page["html"]
    assert not store["alice"].get("email_verified")

    done = _call(ns, "POST", {"node": "alice", "token": token})
    assert done["status"] == 200 and "Email confirmed" in done["html"]
    assert store["alice"]["email_verified"] is True
    assert store["alice"]["email_verification_method"] == "recipient_link_post"
    assert "email_verify_issued_at" not in store["alice"]
    assert ("hackathon", "bi:alice") in events

    again = _call(ns, "POST", {"node": "alice", "token": token})
    assert "Email already confirmed" in again["html"]
    assert len([e for e in events if e[0] == "hackathon"]) == 1


def test_editing_the_url_never_confirms():
    ns, store, _ = _setup()
    token = _link(ns, "alice", "alice@example.com", NOW)
    for params in ({"node": "bob", "token": token},
                   {"node": "alice", "token": token[:-1] + (
                       "A" if token[-1] != "A" else "B")},
                   {"node": "alice", "token": token, "issuedAt": str(NOW)},
                   {"node": "alice", "token": ""},
                   {"node": "nobody", "token": token}):
        result = _call(ns, "POST", params)
        assert result["status"] == 400, params
    assert not store["alice"].get("email_verified")
    assert not store["bob"].get("email_verified")


def test_a_newer_email_replaces_the_older_link():
    ns, store, _ = _setup(email_verify_issued_at=NOW + 60_000)
    old = _link(ns, "alice", "alice@example.com", NOW)
    _Date.now_ms = NOW + 120_000
    replaced = _call(ns, "POST", {"node": "alice", "token": old})
    assert replaced["status"] == 410
    assert "This link was replaced" in replaced["html"]
    assert not store["alice"].get("email_verified")

    new = _link(ns, "alice", "alice@example.com", NOW + 60_000)
    assert _call(ns, "POST", {"node": "alice", "token": new})["status"] == 200
    assert store["alice"]["email_verified"] is True


def test_links_expire():
    ns, store, _ = _setup()
    token = _link(ns, "alice", "alice@example.com", NOW)
    _Date.now_ms = NOW + ns["EMAIL_VERIFICATION_TOKEN_TTL_MS"] + 1
    assert _call(ns, "POST", {"node": "alice", "token": token})["status"] == 400
    assert not store["alice"].get("email_verified")


def test_links_sent_before_the_change_still_confirm():
    ns, store, _ = _setup()
    legacy = asyncio.run(ns["_email_verify_token"](
        None, "alice", "alice@example.com", NOW))
    params = {"node": "alice", "issuedAt": str(NOW), "token": legacy}
    page = _call(ns, "GET", params)
    assert page["status"] == 200
    assert 'name="issuedAt" value="%d"' % NOW in page["html"]
    assert _call(ns, "POST", {**params, "issuedAt": str(NOW + 1)})[
        "status"] == 400
    assert _call(ns, "POST", params)["status"] == 200
    assert store["alice"]["email_verified"] is True

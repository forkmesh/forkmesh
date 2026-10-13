"""Privacy-bounded login continuity for independent mirror origins."""

import base64
import asyncio
from http.client import HTTPMessage
from pathlib import Path
import re
import sys
from types import SimpleNamespace
from urllib.parse import parse_qs, quote, urlparse

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "app" / "src"))

import portable_sessions  # noqa: E402


NOW = 2_000_000_000_000


def _encoded_token(expires_at=NOW + 60_000):
    private = Ed25519PrivateKey.generate()
    claim = portable_sessions.build_claim(
        account_id="a" * 64,
        account_name="alice",
        session_id="s" * 32,
        grant_id="g" * 24,
        issued_at=NOW,
        expires_at=expires_at,
    )
    encoded = portable_sessions.encode_claim(claim)
    signature = private.sign(portable_sessions.signing_message(encoded))
    token = portable_sessions.assemble_token(
        encoded, base64.urlsafe_b64encode(signature).decode().rstrip("=")
    )
    return private.public_key(), token


def test_controller_grant_round_trips_without_credentials_or_d1_rows():
    public, token = _encoded_token()

    def verify(message, signature):
        public.verify(signature, message)
        return True

    claim = portable_sessions.parse_token(token, now_ms=NOW, verify=verify)
    assert claim["name"] == "alice"
    assert claim["sub"] == "a" * 64
    assert not {
        "email", "password", "token", "sessionToken", "totp", "admin"
    } & set(claim)


def test_portable_grant_rejects_tampering_expiry_and_wrong_signer():
    _public, token = _encoded_token()
    pieces = token.split(".")
    tampered = ".".join((pieces[0], pieces[1][:-1] + "A", pieces[2]))
    wrong = Ed25519PrivateKey.generate().public_key()

    def wrong_verify(message, signature):
        wrong.verify(signature, message)
        return True

    with pytest.raises(portable_sessions.PortableSessionError):
        portable_sessions.parse_token(tampered, now_ms=NOW, verify=wrong_verify)
    with pytest.raises(portable_sessions.PortableSessionError, match="expired"):
        portable_sessions.parse_token(
            token,
            now_ms=NOW + portable_sessions.SESSION_CLOCK_SKEW_MS + 60_001,
            verify=lambda _message, _signature: True,
        )


def test_portable_grant_rejects_noncanonical_signature_base64():
    _public, token = _encoded_token()
    prefix, claim, signature = token.split(".")
    alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    raw_signature = base64.urlsafe_b64decode(signature + "==")
    replacement = next(
        character for character in alphabet
        if character != signature[-1]
        and base64.urlsafe_b64decode(
            signature[:-1] + character + "=="
        ) == raw_signature
    )
    with pytest.raises(portable_sessions.PortableSessionError, match="encoding"):
        portable_sessions.parse_token(
            ".".join((prefix, claim, signature[:-1] + replacement)),
            now_ms=NOW,
            verify=lambda _message, _signature: True,
        )


def test_portable_cookie_parser_rejects_duplicate_or_oversized_values():
    _public, token = _encoded_token()
    assert portable_sessions.cookie_value(
        "a=1; forkmesh_portable=" + token + "; b=2"
    ) == token
    assert portable_sessions.cookie_value(
        "forkmesh_portable=" + token + "; forkmesh_portable=" + token
    ) == ""
    assert portable_sessions.cookie_value("x=" + "z" * (17 * 1024)) == ""


def test_portable_cookie_append_accepts_python_worker_http_message():
    """Session refreshes must work after Response crosses Python compat."""
    entry = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    tree = __import__("ast").parse(entry)
    function = next(
        node for node in tree.body
        if isinstance(node, __import__("ast").FunctionDef)
        and node.name == "_append_set_cookie"
    )
    namespace = {}
    exec(__import__("ast").get_source_segment(entry, function), namespace)
    headers = HTTPMessage()
    headers.add_header("Set-Cookie", "forkmesh_account=controller")
    response = SimpleNamespace(headers=headers)

    returned = namespace["_append_set_cookie"](
        response, "forkmesh_portable=mirror")

    assert returned is response
    assert headers.get_all("Set-Cookie") == [
        "forkmesh_account=controller",
        "forkmesh_portable=mirror",
    ]


def _entry_functions(*names):
    ast = __import__("ast")
    entry = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    wanted = set(names)
    return [
        ast.get_source_segment(entry, node)
        for node in ast.parse(entry).body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in wanted
    ]


def test_session_reads_do_not_resign_a_young_portable_grant():
    """Load/focus reads reuse the browser's grant instead of signing again."""
    import json

    _public, token = _encoded_token(expires_at=NOW + 6 * 60 * 60 * 1000)
    session_token = "v2." + "s" * 32 + "." + "x" * 43
    signed = []

    async def portable(_env, _token, _rec):
        signed.append(True)
        return "v1.fresh", NOW + 1000

    clock = {"now": NOW + 60_000}
    namespace = {
        "ACCOUNT_SESSION_TOKEN_RE": re.compile(
            r"^v2\.([A-Za-z0-9_-]{24,64})\.([A-Za-z0-9_-]{32,96})$"),
        "Date": SimpleNamespace(now=lambda: clock["now"]),
        "MAX_NODE_NAME": 63,
        "PORTABLE_SESSION_COOKIE": "forkmesh_portable",
        "PORTABLE_SESSION_REISSUE_MS": 4 * 60 * 60 * 1000,
        "_append_set_cookie": lambda response, value: response.cookies.append(
            value),
        "_portable_session_cookie": lambda *args: "forkmesh_portable=fresh",
        "_portable_session_cookie_domain": lambda _env, _request: (
            ".forkmesh.com"),
        "_portable_session_token": portable,
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "json": json,
        "portable_sessions": portable_sessions,
    }
    for source in _entry_functions(
            "_portable_session_cookie_current",
            "_append_portable_session_cookie"):
        exec(source, namespace)
    append = namespace["_append_portable_session_cookie"]

    def run(cookie, rec=None, session=session_token, **kwargs):
        response = SimpleNamespace(headers={}, cookies=[])
        request = SimpleNamespace(headers={"cookie": cookie})
        asyncio.run(append(
            object(), request, response, session,
            rec or {"name": "alice"}, **kwargs))
        return response

    held = "forkmesh_account=x; forkmesh_portable=" + token
    reused = run(held, reuse_current=True)
    assert reused.headers["X-ForkMesh-Portable-Session"] == "current"
    assert reused.cookies == [] and signed == []

    # Login (no reuse flag), a missing cookie, another account's or another
    # session's grant, and a grant past the refresh window are all re-signed.
    assert run(held).headers["X-ForkMesh-Portable-Session"] == "issued"
    assert run("", reuse_current=True).cookies == ["forkmesh_portable=fresh"]
    assert run(held, rec={"name": "mallory"}, reuse_current=True).cookies
    other = "v2." + "t" * 32 + "." + "x" * 43
    assert run(held, session=other, reuse_current=True).cookies
    clock["now"] = NOW + 4 * 60 * 60 * 1000
    assert run(held, reuse_current=True).cookies
    assert len(signed) == 5


def test_controller_handoff_posts_grant_only_to_official_mirror_body():
    entry = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    tree = __import__("ast").parse(entry)
    function = next(
        node for node in tree.body
        if isinstance(node, __import__("ast").AsyncFunctionDef)
        and node.name == "_account_mirror_handoff"
    )
    lookups = []

    async def lookup(_env, token, touch=True):
        lookups.append((token, touch))
        return "account", {"name": "alice"}, "s" * 32

    async def portable(_env, token, _rec):
        assert token == "controller-cookie"
        return "signed-portable-grant", NOW + 60_000

    class FakeResponse:
        def __init__(self, body, status=200, headers=None):
            self.body = body
            self.status = status
            self.headers = headers or {}

    namespace = {
        "Response": FakeResponse,
        "_account_session_invalid_error": lambda *_args: None,
        "_account_session_lookup": lookup,
        "_b64url_encode": lambda _raw: "page-nonce",
        "_html_escape": lambda value: (
            str(value).replace("&", "&amp;").replace('"', "&quot;")),
        "_portable_session_token": portable,
        "_random_bytes": lambda _length: b"nonce",
        "_request_account_session_token": (
            lambda _request, _body: ("controller-cookie", True)),
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "json_response": lambda body, status=200, **_kwargs: FakeResponse(
            body, status=status),
        "method_name": lambda request: request.method,
        "parse_qs": parse_qs,
        "re": re,
        "safe_segment": lambda value: value if "/" not in value else "",
        "urlparse": urlparse,
        "valid_node_name": lambda value: bool(re.fullmatch(
            r"[a-z0-9][a-z0-9-]{0,62}", value)),
    }
    exec(__import__("ast").get_source_segment(entry, function), namespace)
    destination = (
        "https://mirror37.forkmesh.com/forkmesh/forkmesh?theme=dark"
    )
    response = asyncio.run(namespace["_account_mirror_handoff"](
        object(),
        SimpleNamespace(
            method="GET",
            url="https://app.forkmesh.com/api/accounts/mirror-handoff?to="
            + quote(destination, safe=""),
        ),
    ))
    assert response.status == 200
    assert lookups == [("controller-cookie", True)]
    assert 'method="post"' in response.body
    assert (
        'action="https://mirror37.forkmesh.com/api/accounts/session-handoff"'
        in response.body
    )
    assert 'value="signed-portable-grant"' in response.body
    assert 'value="/forkmesh/forkmesh?theme=dark"' in response.body
    assert "signed-portable-grant" not in response.headers.get(
        "location", "")
    assert response.headers["cache-control"].startswith("no-store")
    assert response.headers["referrer-policy"] == "no-referrer"

    rejected = asyncio.run(namespace["_account_mirror_handoff"](
        object(),
        SimpleNamespace(
            method="GET",
            url=(
                "https://app.forkmesh.com/api/accounts/mirror-handoff?to="
                "https%3A%2F%2Fattacker.example%2Fforkmesh%2Fforkmesh"
            ),
        ),
    ))
    assert rejected.status == 400

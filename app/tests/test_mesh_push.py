import asyncio
import importlib.util
from pathlib import Path
from urllib.parse import parse_qs, urlencode, urlparse


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY = (SRC / "entry.py").read_text(encoding="utf-8")
APP_RUNTIME = (SRC / "app_runtime.py").read_text(encoding="utf-8")
FOOTPRINT = (
    ROOT.parent / "world" / "tools" / "build_worker_footprint.py"
).read_text(encoding="utf-8")
NOW = 1_760_000_000_000


class _Date:
    now_ms = NOW

    @classmethod
    def now(cls):
        return cls.now_ms


class _Request:
    def __init__(self, url, body, method="POST"):
        self.url = url
        self.body = body
        self.method = method


class _Apns:
    APNS_CHAT_TOPIC = "chat"
    APNS_MAX_DEVICES_PER_PUSH = 10
    APNS_MESH_CONVERSATION = "#mesh"

    def __init__(self, configured=True):
        self.configured = configured

    def apns_configured(self, env):
        return self.configured

    @staticmethod
    def chat_message_alert(channel_name="", conversation=""):
        return {"title": "New message in #" + channel_name, "body": "sealed",
                "conversation": conversation}


def _bi(name):
    return (name.encode().hex() * 64)[:64]


def _load(session_account="", signature_ok=True, configured=True,
          devices=None):
    spec = importlib.util.spec_from_file_location(
        "mesh_push_under_test", SRC / "mesh_push.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    calls = {"sent": [], "verified": [], "reads": []}
    apns = _Apns(configured)
    devices = devices if devices is not None else {
        _bi("blue"): [{"token": "aa", "token_bi": "t1"}],
        _bi("jett"): [{"token": "bb", "token_bi": "t2"}],
        _bi("fungi"): [{"token": "cc", "token_bi": "t3"}],
    }

    async def ensure_schema(env):
        return None

    async def bounded_json_request(request):
        return request.body

    def json_response(payload, status=200, **kwargs):
        return {"status": status, "payload": payload}

    async def _authed_account_name(env, request, data=None):
        return session_account

    async def _verify_owner_signature(env, user, sig, canonical):
        calls["verified"].append((user, sig, canonical))
        return signature_ok

    async def _account_row(env, name):
        return ("bi", {"status": "active"})

    async def blind_index(env, value):
        return _bi(value)

    async def d1_all(env, sql, account_bi, limit):
        calls["reads"].append(account_bi)
        return devices.get(account_bi, [])

    async def _apns_tokens_from_rows(env, rows):
        return list(rows)

    async def _apns_send_tokens(env, tokens, topic, alert):
        calls["sent"].append((tuple(t["token"] for t in tokens), topic, alert))
        return len(tokens)

    module._bind_runtime({
        "Date": _Date,
        "urlparse": urlparse,
        "parse_qs": parse_qs,
        "ensure_schema": ensure_schema,
        "bounded_json_request": bounded_json_request,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda name: bool(name) and name.isalnum(),
        "MAX_NODE_NAME": 64,
        "_ts_ok": lambda ts: str(ts).isdigit(),
        "_require_data_secret": lambda env: "data-secret",
        "_authed_account_name": _authed_account_name,
        "_verify_owner_signature": _verify_owner_signature,
        "_account_row": _account_row,
        "blind_index": blind_index,
        "d1_all": d1_all,
        "_apns_module": lambda: apns,
        "_apns_tokens_from_rows": _apns_tokens_from_rows,
        "_apns_send_tokens": _apns_send_tokens,
    })
    module._MESH_WAKE_MEMO.clear()
    return module, calls


def _call(handler, path, body, query=None, method="POST"):
    url = "https://app.forkmesh.com" + path
    if query:
        url += "?" + urlencode(query)
    return asyncio.run(handler(None, _Request(url, body, method)))


def _token(module, name, now=NOW):
    return module.mesh_wake_token(None, _bi(name), now)[0]


def test_routes_and_lazy_module_are_wired():
    assert '_mesh_push = _LazyModule("mesh_push")' in ENTRY
    assert ('mesh_wake_handler = _mesh_push.export("mesh_wake_handler")'
            in ENTRY)
    assert ('mesh_wake_mint_handler = _mesh_push.export('
            '"mesh_wake_mint_handler")') in ENTRY
    assert "return await mesh_wake_handler(self.env, request)" in APP_RUNTIME
    assert ("return await mesh_wake_mint_handler(self.env, request)"
            in APP_RUNTIME)
    assert '"/api/push/mesh"' in APP_RUNTIME
    assert '"/api/push/mesh-wake"' in APP_RUNTIME
    assert '"mesh_push.py",' in FOOTPRINT


def test_mint_needs_an_account_and_names_only_its_blind_index():
    module, _ = _load()
    denied = _call(module.mesh_wake_mint_handler, "/api/push/mesh-wake", {})
    assert denied["status"] == 401

    module, _ = _load(session_account="blue")
    minted = _call(module.mesh_wake_mint_handler, "/api/push/mesh-wake", {})
    assert minted["status"] == 200
    token = minted["payload"]["token"]
    assert "blue" not in token
    assert token.split(".")[1] == _bi("blue")
    assert minted["payload"]["expiresAt"] == (
        NOW + module.MESH_WAKE_TOKEN_TTL_MS)
    assert module.mesh_wake_token_account(None, token, NOW) == _bi("blue")


def test_desktop_mints_with_a_signed_request():
    module, calls = _load(session_account="")
    minted = _call(module.mesh_wake_mint_handler, "/api/push/mesh-wake", {},
                   {"user": "jett", "ts": "123", "sig": "proof"})
    assert minted["status"] == 200
    assert calls["verified"] == [
        ("jett", "proof", b"forkmesh-mesh-wake-mint-v1\njett\n123")]


def test_forged_expired_and_far_future_tokens_name_nobody():
    module, _ = _load()
    token = _token(module, "blue")
    prefix, account_bi, expires, signature = token.split(".")
    forged = ".".join((prefix, _bi("fungi"), expires, signature))
    assert module.mesh_wake_token_account(None, forged, NOW) == ""
    tampered = token[:-1] + ("0" if token[-1] != "0" else "1")
    assert module.mesh_wake_token_account(None, tampered, NOW) == ""
    later = NOW + module.MESH_WAKE_TOKEN_TTL_MS
    assert module.mesh_wake_token_account(None, token, later) == ""
    future = _token(module, "blue", NOW + 60_000)
    assert module.mesh_wake_token_account(None, future, NOW) == ""
    assert module.mesh_wake_token_account(None, "mw1.x.y.z", NOW) == ""
    assert module.mesh_wake_token_account(None, None, NOW) == ""


def test_wake_pushes_each_token_owner_once_and_never_the_sender():
    module, calls = _load(session_account="jett")
    tokens = [_token(module, "blue"), _token(module, "jett"),
              _token(module, "blue", NOW - 1000), _token(module, "fungi")]
    woke = _call(module.mesh_wake_handler, "/api/push/mesh",
                 {"tokens": tokens})
    assert woke == {"status": 200, "payload": {"ok": True, "woken": 2}}
    assert calls["reads"] == [_bi("blue"), _bi("fungi")]
    alert = {"title": "New message in #mesh", "body": "sealed",
             "conversation": "#mesh"}
    assert calls["sent"] == [(("aa",), "chat", alert), (("cc",), "chat", alert)]


def test_wake_coalesces_a_burst_per_recipient():
    module, calls = _load(session_account="jett")
    body = {"tokens": [_token(module, "blue")]}
    _call(module.mesh_wake_handler, "/api/push/mesh", body)
    again = _call(module.mesh_wake_handler, "/api/push/mesh", body)
    assert again["payload"]["woken"] == 0
    assert len(calls["sent"]) == 1
    _Date.now_ms = NOW + module.MESH_WAKE_COALESCE_MS
    try:
        later = _call(module.mesh_wake_handler, "/api/push/mesh", body)
    finally:
        _Date.now_ms = NOW
    assert later["payload"]["woken"] == 1


def test_wake_rejects_anonymous_and_malformed_requests():
    module, calls = _load(session_account="", signature_ok=False)
    tokens = [_token(module, "blue")]
    anonymous = _call(module.mesh_wake_handler, "/api/push/mesh",
                      {"tokens": tokens},
                      {"user": "jett", "ts": "123", "sig": "bad"})
    assert anonymous["status"] == 401
    assert calls["sent"] == []
    for body in ({}, {"tokens": "x"}, {"tokens": [1]},
                 {"tokens": ["t"] * 33}):
        assert _call(module.mesh_wake_handler, "/api/push/mesh",
                     body)["status"] == 400
    assert _call(module.mesh_wake_handler, "/api/push/mesh", {"tokens": []},
                 method="GET")["status"] == 405


def test_desktop_signs_the_token_list():
    module, calls = _load(session_account="")
    tokens = [_token(module, "blue"), _token(module, "fungi")]
    woke = _call(module.mesh_wake_handler, "/api/push/mesh",
                 {"tokens": tokens},
                 {"user": "jett", "ts": "123", "sig": "proof"})
    assert woke["payload"]["woken"] == 2
    expected = ("forkmesh-mesh-wake-v1\njett\n" + "\n".join(tokens)
                + "\n123")
    assert calls["verified"] == [("jett", "proof", expected.encode())]


def test_wake_without_provider_credentials_is_a_quiet_no_op():
    module, calls = _load(session_account="jett", configured=False)
    woke = _call(module.mesh_wake_handler, "/api/push/mesh",
                 {"tokens": [_token(module, "blue")]})
    assert woke["payload"] == {"ok": True, "woken": 0,
                               "pushConfigured": False}
    assert calls["sent"] == []

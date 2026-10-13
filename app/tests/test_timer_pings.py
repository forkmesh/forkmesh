"""Desktop timer expiries become account pings (POST /api/notifications/timer).

The ping is what reaches the phone: enqueue_notification rings the pings
topic, and that topic is pushed over APNs.
"""

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
DESKTOP_TIMERS = (
    ROOT.parent / "desktop" / "src" / "MainWindowTimers.cpp"
).read_text(encoding="utf-8")
DESKTOP_INTERNAL = (
    ROOT.parent / "desktop" / "src" / "MainWindowInternal.h"
).read_text(encoding="utf-8")
NOW = 1_760_000_000_000


class _Date:
    @staticmethod
    def now():
        return NOW


class _Request:
    def __init__(self, url, method="POST"):
        self.url = url
        self.method = method


def _load(session_account="", signature_ok=True, account=None):
    spec = importlib.util.spec_from_file_location(
        "timer_pings_under_test", SRC / "timer_pings.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    calls = {"enqueued": [], "verified": []}

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
        record = account if account is not None else {"status": "active"}
        return ("bi", record)

    async def enqueue_notification(env, recipient, kind, title, **kwargs):
        calls["enqueued"].append((recipient, kind, title, kwargs))
        return True

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
        "_authed_account_name": _authed_account_name,
        "_verify_owner_signature": _verify_owner_signature,
        "_account_row": _account_row,
        "enqueue_notification": enqueue_notification,
    })
    return module, calls


def _post(module, body, query=None, method="POST"):
    url = "https://app.forkmesh.com/api/notifications/timer"
    if query:
        url += "?" + urlencode(query)
    request = _Request(url, method)
    request.body = body
    return asyncio.run(module.timer_ping_handler(None, request))


def _body(**overrides):
    body = {"timerId": "a1b2c3d4-e5f", "kind": "countdown",
            "endsAt": NOW - 1000, "label": "tea"}
    body.update(overrides)
    return body


def test_route_kind_and_lazy_module_are_wired():
    assert '"timer_finished",' in ENTRY
    assert '_timer_pings = _LazyModule("timer_pings")' in ENTRY
    assert ('timer_ping_handler = _timer_pings.export("timer_ping_handler")'
            in ENTRY)
    assert '"/api/notifications/timer"' in APP_RUNTIME
    assert "return await timer_ping_handler(self.env, request)" in APP_RUNTIME
    assert '"timer_pings.py",' in FOOTPRINT


def test_desktop_signs_the_same_proof_and_sends_the_ping():
    module, _ = _load()
    assert (f'QStringLiteral("{module.TIMER_PING_PROOF}")' in DESKTOP_INTERNAL)
    assert "/api/notifications/timer" in DESKTOP_TIMERS
    assert "sendTimerPing(timer, title, body)" in DESKTOP_TIMERS
    # Kept in step with the relay's refusal window.
    assert "kTimerPingMaxAgeMs = 15 * 60 * 1000" in DESKTOP_TIMERS
    assert module.TIMER_PING_MAX_AGE_MS == 15 * 60 * 1000


def test_signed_expiry_pings_the_signer_only():
    module, calls = _load()
    canonical = module.timer_ping_canonical(
        "jett", "123", "a1b2c3d4-e5f", "countdown", NOW - 1000, "tea")
    assert canonical == (
        b"forkmesh-account-timer-ping-v1\njett\na1b2c3d4-e5f\ncountdown\n"
        + str(NOW - 1000).encode() + b"\ntea\n123")
    response = _post(module, _body(),
                     {"user": "jett", "ts": "123", "sig": "proof"})
    assert response["status"] == 200
    assert calls["verified"] == [("jett", "proof", canonical)]
    [(recipient, kind, title, kwargs)] = calls["enqueued"]
    assert (recipient, kind, title) == ("jett", "timer_finished",
                                        "Countdown finished")
    assert kwargs["body"] == "tea"
    assert kwargs["dedupe"] == "timer:a1b2c3d4-e5f:" + str(NOW - 1000)
    # Filed at the moment it lands: the phone's notification extension only
    # names rows from the last hour.
    assert kwargs["ts"] == NOW
    assert kwargs["meta"]["timerKind"] == "countdown"


def test_alarm_without_label_gets_a_worker_written_body():
    module, calls = _load(session_account="jett")
    response = _post(module, _body(kind="alarm", label=""))
    assert response["status"] == 200
    [(_, _, title, kwargs)] = calls["enqueued"]
    assert title == "Alarm"
    assert kwargs["body"] == "Your alarm is ringing."
    assert calls["verified"] == []  # the session was enough


def test_session_is_read_before_the_signed_triple():
    module, calls = _load(session_account="jett", signature_ok=False)
    assert _post(module, _body(),
                 {"user": "mallory", "ts": "123", "sig": "x"})["status"] == 200
    assert calls["enqueued"][0][0] == "jett"


def test_refusals():
    module, calls = _load(signature_ok=False)
    signed = {"user": "jett", "ts": "123", "sig": "proof"}
    assert _post(module, _body(), signed)["status"] == 401
    assert _post(module, _body())["status"] == 401
    assert _post(module, _body(), signed, method="GET")["status"] == 405

    module, calls = _load()
    assert _post(module, _body(kind="timer"), signed)["status"] == 400
    assert _post(module, _body(timerId="../x"), signed)["status"] == 400
    assert _post(module, _body(label="x" * 81), signed)["status"] == 400
    assert _post(module, _body(endsAt=True), signed)["status"] == 400
    stale = _post(module, _body(endsAt=NOW - 16 * 60 * 1000), signed)
    assert stale["status"] == 409
    future = _post(module, _body(endsAt=NOW + 10 * 60 * 1000), signed)
    assert future["status"] == 409
    assert calls["enqueued"] == []

    module, calls = _load(account={"status": "suspended"})
    assert _post(module, _body(), signed)["status"] == 401
    assert calls["enqueued"] == []

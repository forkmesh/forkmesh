import asyncio
import importlib.util
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
NOW = 1_800_000_000_000
HOUR = 60 * 60 * 1000
DAY = 24 * HOUR
WEEK = 7 * DAY


def _load():
    sys.path.insert(0, str(REPO / "world" / "src"))
    try:
        import world as world_protocol
    finally:
        sys.path.pop(0)
    spec = importlib.util.spec_from_file_location(
        "account_chat_retention_under_test",
        ROOT / "src" / "account_status.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    saved = []

    class _Date:
        @staticmethod
        def now():
            return NOW

    async def _save_account(env, name_bi, rec):
        saved.append((name_bi, dict(rec)))

    module._bind_runtime({
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "Date": _Date,
        "world_protocol": world_protocol,
        "json_response": lambda payload, status=200: (status, payload),
        "_save_account": _save_account,
    })
    return module, saved


def _post(module, rec, body):
    async def _bounded_json_request(request):
        return body

    async def _session(env, request, data=None):
        return ("bi", rec) if rec is not None else ("", None)

    module.bounded_json_request = _bounded_json_request
    module._account_session_record = _session
    return asyncio.run(module.account_chat_retention_route(None, object()))


def test_entry_routes_and_returns_chat_retention_to_the_owner():
    assert '"/api/accounts/chat-retention" and method == "POST"' in ENTRY
    assert "_account_status.account_chat_retention_route(env, request)" in ENTRY
    assert '"chatRetention": _account_chat_retention(rec)' in ENTRY


def test_default_is_seven_days_and_a_read_does_not_write():
    module, saved = _load()
    rec = {"name": "alice"}
    status, payload = _post(module, rec, {})
    assert status == 200
    assert payload["chatRetention"] == {
        "defaultMs": WEEK, "chats": {}, "updatedAt": 0}
    assert payload["nodeName"] == "alice"
    assert saved == []


def test_default_and_per_chat_overrides_are_stored_and_clamped():
    module, saved = _load()
    rec = {"name": "alice"}
    status, payload = _post(module, rec, {"defaultMs": DAY})
    assert status == 200
    assert payload["chatRetention"]["defaultMs"] == DAY

    status, payload = _post(
        module, rec, {"chat": "#general", "retentionMs": HOUR})
    assert status == 200
    assert payload["chatRetention"]["chats"] == {"#general": HOUR}
    assert payload["chatRetention"]["defaultMs"] == DAY
    assert payload["chatRetention"]["updatedAt"] == NOW

    status, payload = _post(
        module, rec, {"chat": "direct:" + "a" * 32, "retentionMs": 30 * DAY})
    assert payload["chatRetention"]["chats"]["direct:" + "a" * 32] == WEEK

    status, payload = _post(
        module, rec, {"chat": "channel:" + "b" * 32, "retentionMs": 1000})
    assert payload["chatRetention"]["chats"]["channel:" + "b" * 32] == HOUR
    assert len(saved) == 4


def test_null_clears_an_override_and_the_default():
    module, saved = _load()
    rec = {"name": "alice", "chat_retention": {
        "defaultMs": DAY, "chats": {"#general": HOUR}, "updatedAt": 1}}
    status, payload = _post(module, rec, {"chat": "#general", "retentionMs": None})
    assert status == 200
    assert payload["chatRetention"]["chats"] == {}
    assert payload["chatRetention"]["defaultMs"] == DAY
    status, payload = _post(module, rec, {"defaultMs": None})
    assert payload["chatRetention"]["defaultMs"] == WEEK
    assert "chat_retention" not in rec
    assert len(saved) == 2


def test_setting_the_default_to_seven_days_stores_nothing():
    module, saved = _load()
    rec = {"name": "alice"}
    status, payload = _post(module, rec, {"defaultMs": WEEK})
    assert status == 200
    assert payload["chatRetention"]["defaultMs"] == WEEK
    assert saved == []
    assert "chat_retention" not in rec


@pytest.mark.parametrize("body", [
    {"defaultMs": "soon"},
    {"defaultMs": 0},
    {"defaultMs": True},
    {"chat": "", "retentionMs": HOUR},
    {"chat": "x" * 161, "retentionMs": HOUR},
    {"chat": "#bad\nkey", "retentionMs": HOUR},
    {"retentionMs": HOUR},
    {"chat": "#general", "retentionMs": -5},
])
def test_bad_requests_are_refused(body):
    module, saved = _load()
    status, payload = _post(module, {"name": "alice"}, body)
    assert status == 400
    assert payload["error"] == "invalid_chat_retention"
    assert saved == []


def test_signed_out_callers_are_refused():
    module, _ = _load()
    status, payload = _post(module, None, {})
    assert status == 401


def test_override_count_is_bounded():
    module, _ = _load()
    chats = {f"#c{i}": HOUR for i in range(module.CHAT_RETENTION_MAX_CHATS)}
    rec = {"name": "alice", "chat_retention": {"chats": chats}}
    status, payload = _post(module, rec, {"chat": "#one-more", "retentionMs": HOUR})
    assert status == 400
    assert payload["error"] == "too_many_chat_retention_overrides"
    status, payload = _post(module, rec, {"chat": "#c1", "retentionMs": DAY})
    assert status == 200


def test_corrupt_stored_values_read_as_defaults():
    module, _ = _load()
    view = module.account_chat_retention({"chat_retention": {
        "defaultMs": "x", "chats": {"#ok": DAY, "#bad": "y", 5: HOUR}}})
    assert view == {"defaultMs": WEEK, "chats": {"#ok": DAY}, "updatedAt": 0}

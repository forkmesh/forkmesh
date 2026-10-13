"""Away flag + custom status: POST /api/accounts/user-status and its payloads."""

import asyncio
import importlib.util
import json
import shutil
import subprocess
import sys
from pathlib import Path
from urllib.parse import quote

import pytest


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
FOOTPRINT = (REPO / "world" / "tools" / "build_worker_footprint.py").read_text(
    encoding="utf-8")
NOW = 1_800_000_000_000


def _load(overrides=None):
    sys.path.insert(0, str(REPO / "world" / "src"))
    try:
        import world as world_protocol
    finally:
        sys.path.pop(0)
    spec = importlib.util.spec_from_file_location(
        "account_status_under_test", ROOT / "src" / "account_status.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    saved = []
    purged = []

    class _Date:
        @staticmethod
        def now():
            return NOW

    async def _save_account(env, name_bi, rec):
        saved.append((name_bi, dict(rec)))

    async def _edge_cache_delete(key):
        purged.append(key)

    runtime = {
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "Date": _Date,
        "world_protocol": world_protocol,
        "json_response": lambda payload, status=200: (status, payload),
        "_save_account": _save_account,
        "edge_cache_delete": _edge_cache_delete,
        "quote": quote,
        "ACCOUNT_LOOKUP_CACHE_PREFIX": "https://lookup/",
        "USERS_DIRECTORY_CACHE_KEY": "https://directory",
        "USERS_WORLD_CACHE_KEY": "https://directory?view=world",
        "MAX_NODE_NAME": 39,
        **(overrides or {}),
    }
    module._bind_runtime(runtime)
    return module, saved, purged


def _post(module, rec, body):
    async def _bounded_json_request(request):
        return body

    async def _session(env, request, data=None):
        return ("bi", rec) if rec is not None else ("", None)

    module._bind_runtime({})
    module.bounded_json_request = _bounded_json_request
    module._account_session_record = _session
    return asyncio.run(module.account_user_status(None, object()))


def test_entry_routes_and_exposes_user_status_lazily():
    assert '_account_status = _LazyModule("account_status")' in ENTRY
    assert '"/api/accounts/user-status" and method == "POST"' in ENTRY
    assert ENTRY.count('"userStatus": _public_user_status(rec)') == 2
    assert '"account_status.py"' in FOOTPRINT


def test_public_status_drops_expired_custom_status_but_keeps_away():
    module, _, _ = _load()
    rec = {"user_status": {"away": True, "emoji": "🌴", "text": "Holiday",
                           "expiresAt": NOW - 1}}
    assert module.public_user_status(rec) == {
        "away": True, "emoji": "", "text": "", "expiresAt": 0}
    rec["user_status"]["expiresAt"] = NOW + 60_000
    assert module.public_user_status(rec)["emoji"] == "🌴"
    assert module.public_user_status({}) == {
        "away": False, "emoji": "", "text": "", "expiresAt": 0}


def test_post_sets_status_and_purges_profile_and_directory_caches():
    module, saved, purged = _load()
    rec = {"name": "Alice"}
    status, payload = _post(module, rec, {
        "emoji": "🤒", "text": "  Out\nsick  ", "expiresAt": NOW + 3_600_000})
    assert status == 200
    assert payload["userStatus"] == {
        "away": False, "emoji": "🤒", "text": "Out sick",
        "expiresAt": NOW + 3_600_000}
    assert saved and saved[-1][1]["user_status"]["updatedAt"] == NOW
    assert purged == [
        "https://lookup/alice", "https://directory",
        "https://directory?view=world"]

    # Setting away leaves the custom status alone; clear leaves away alone.
    _, payload = _post(module, rec, {"away": True})
    assert payload["userStatus"]["away"] is True
    assert payload["userStatus"]["emoji"] == "🤒"
    _, payload = _post(module, rec, {"clear": True})
    assert payload["userStatus"] == {
        "away": True, "emoji": "", "text": "", "expiresAt": 0}
    _, payload = _post(module, rec, {"away": False})
    assert "user_status" not in rec


def test_post_rejects_bad_input_and_unauthenticated_callers():
    module, saved, _ = _load()
    assert _post(module, None, {"away": True})[0] == 401
    rec = {"name": "bob"}
    assert _post(module, rec, {"emoji": "hello"}) == (
        400, {"error": "bad_status_emoji"})
    assert _post(module, rec, {"text": "x", "expiresAt": NOW - 5}) == (
        400, {"error": "status_expiry_in_past"})
    assert _post(module, rec, {"text": "x", "expiresAt": NOW * 2}) == (
        400, {"error": "status_expiry_too_far"})
    assert _post(module, rec, {"text": "x" * 500}) == (
        400, {"error": "status_text_too_long"})
    assert not saved


def test_empty_post_reads_without_writing():
    module, saved, _ = _load()
    rec = {"name": "carol",
           "user_status": {"away": True, "emoji": "", "text": "",
                           "expiresAt": 0}}
    status, payload = _post(module, rec, {})
    assert status == 200 and payload["userStatus"]["away"] is True
    assert not saved


# ---- web client (app/public/user-status.js) ---------------------------------

PUBLIC = ROOT / "public"
USER_STATUS_JS = PUBLIC / "user-status.js"

_NODE_HARNESS = r"""
const fs = require("fs");
const vm = require("vm");
const listeners = {};
const storage = { "forkmesh.session": JSON.stringify({ nodeName: "Alice", kind: "user" }) };
const element = () => ({
  className: "", textContent: "", title: "", attrs: {},
  setAttribute(k, v) { this.attrs[k] = v; }, append() {},
});
const sandbox = {
  window: {
    dispatchEvent() {},
    addEventListener(name, fn) { listeners[name] = fn; },
  },
  document: {
    getElementById() { return null; },
    createElement: element,
    head: { append() {} },
  },
  localStorage: { getItem: (k) => storage[k] || null },
  CustomEvent: function (name, init) { this.type = name; this.detail = init && init.detail; },
  setTimeout: () => 1, clearTimeout() {}, Date, JSON, Math, Number, String,
  Object, Boolean, Map, Set, Error,
};
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(process.argv[1], "utf8"), sandbox);
const S = sandbox.window.ForkMeshUserStatus;
const now = Date.now();
const out = {};
out.expired = S.current({ away: true, emoji: "🌴", text: "x", expiresAt: now - 1 });
S.remember("Bob", { emoji: "🤒", text: "Out sick", expiresAt: now + 3600000 });
out.bob = S.forName("bob");
out.badge = S.badge(S.forName("BOB")).textContent;
// A directory read right after a live frame must not roll the frame back.
S.remember("carol", { away: true }, "frame");
S.remember("carol", { away: false });
out.carol = S.forName("carol").away;
S.remember("alice", { away: true });
out.mine = S.mine().away;
const noon = new Date(2026, 8, 16, 12, 0, 0); // a Wednesday
out.hour = S.expiresAtFor("1h", "", noon) - noon.getTime();
const today = new Date(S.expiresAtFor("today", "", noon));
out.today = [today.getDate(), today.getHours(), today.getMinutes()];
const week = new Date(S.expiresAtFor("week", "", noon));
out.week = [week.getDay(), week.getDate(), week.getHours()];
out.never = S.expiresAtFor("never", "", noon);
out.customMissing = S.expiresAtFor("custom", "", noon);
out.describeAway = S.describe({ away: true });
console.log(JSON.stringify(out));
"""


def test_web_helper_expiry_frames_and_clear_after_math():
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node.js is unavailable")
    result = subprocess.run(
        [node, "-e", _NODE_HARNESS, str(USER_STATUS_JS)],
        capture_output=True, text=True, timeout=30, check=True)
    out = json.loads(result.stdout)
    assert out["expired"] == {
        "away": True, "emoji": "", "text": "", "expiresAt": 0}
    assert out["bob"]["emoji"] == "🤒" and out["badge"] == "🤒"
    assert out["carol"] is True
    assert out["mine"] is True
    assert out["hour"] == 3_600_000
    assert out["today"] == [16, 23, 59]
    assert out["week"] == [0, 20, 23]
    assert out["never"] == 0
    assert out["customMissing"] == -1
    assert out["describeAway"] == "Away"


def test_web_surfaces_load_the_helper_and_carry_status_on_beats():
    chat_html = (PUBLIC / "chat.html").read_text(encoding="utf-8")
    shell = (PUBLIC / "dashboard" / "shell.html").read_text(encoding="utf-8")
    chat_js = (PUBLIC / "chat.js").read_text(encoding="utf-8")
    dashboard_chat = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
    shell_py = (ROOT / "src" / "dashboard_shell.py").read_text(encoding="utf-8")
    header = (REPO / "www" / "public" / "site-header.js").read_text(
        encoding="utf-8")
    assert '<script src="/user-status.js?v=${version}" defer></script>' in chat_html
    assert chat_html.index("/user-status.js") < chat_html.index("/site-header.js")
    assert shell.index("/user-status.js") < shell.index("/dashboard.js")
    assert '"user-status.js",' in shell_py
    for source in (chat_js, dashboard_chat):
        assert "ownUserStatusFrameField(type)" in source
        assert 'remember(' in source and '"frame")' in source
        assert "forkmesh:user-status-self" in source
    assert "window.ForkMeshUserStatus" in header
    assert "Set yourself away" in header

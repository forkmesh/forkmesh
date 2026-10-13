import json
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
PUBLIC = ROOT / "public"
RETENTION = PUBLIC / "chat-retention.js"
CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
CHAT_HTML = (PUBLIC / "chat.html").read_text(encoding="utf-8")
DASHBOARD_CHAT = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
HOUR = 60 * 60 * 1000
DAY = 24 * HOUR
WEEK = 7 * DAY


def _run_module(script):
    if not shutil.which("node"):
        pytest.skip("node is not installed")
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_retention_module_defaults_overrides_and_sync():
    result = _run_module(f"""
      const m = await import({json.dumps(RETENTION.as_uri())});
      const store = new Map();
      const storage = {{
        getItem: (key) => store.get(key) ?? null,
        setItem: (key, value) => store.set(key, value),
      }};
      const sent = [];
      let server = {{ defaultMs: {WEEK}, chats: {{}}, updatedAt: 0 }};
      const request = async (body) => {{
        sent.push(body);
        if ("defaultMs" in body) server = {{ ...server, defaultMs: body.defaultMs || {WEEK} }};
        if ("chat" in body) {{
          const chats = {{ ...server.chats }};
          if (body.retentionMs) chats[body.chat] = body.retentionMs;
          else delete chats[body.chat];
          server = {{ ...server, chats }};
        }}
        return {{ ok: true, chatRetention: server }};
      }};
      const now = 10 * {DAY};
      const retention = m.createChatRetention({{ request, storage, now: () => now }});
      const changes = [];
      retention.onChange(() => changes.push(retention.defaultMs()));
      const out = {{}};
      out.initialDefault = retention.defaultMs();
      out.initialGeneral = retention.effectiveMs("#general");
      await retention.setChat("private:" + "a".repeat(32), {HOUR});
      out.privateKey = sent.at(-1).chat;
      out.privateEffective = retention.effectiveMs("private:" + "a".repeat(32));
      out.expiredOld = retention.isExpired(now - 2 * {HOUR}, "private:" + "a".repeat(32));
      out.keptNew = retention.isExpired(now - 30 * 60 * 1000, "private:" + "a".repeat(32));
      await retention.setDefault({DAY});
      out.generalAfterDefault = retention.effectiveMs("#general");
      out.dmAfterDefault = retention.effectiveMs("direct:" + "b".repeat(32));
      await retention.setChat("private:" + "a".repeat(32), null);
      out.clearedBody = sent.at(-1);
      out.privateAfterClear = retention.effectiveMs("private:" + "a".repeat(32));
      out.cached = JSON.parse(store.get(m.CHAT_RETENTION_STORAGE_KEY)).defaultMs;
      const reloaded = m.createChatRetention({{ storage, now: () => now }});
      out.reloadedDefault = reloaded.defaultMs();
      out.changes = changes.length;
      out.clampLow = m.clampChatRetentionMs(5);
      out.clampHigh = m.clampChatRetentionMs(30 * {DAY});
      out.clampBad = m.clampChatRetentionMs("x");
      out.labels = m.CHAT_RETENTION_PRESETS.map((p) => p.label);
      out.presetMs = m.CHAT_RETENTION_PRESETS.map((p) => p.ms);
      out.corrupt = m.normalizeChatRetention({{ defaultMs: "x", chats: {{ "#a": "y", "#b": {DAY} }} }});
      console.log(JSON.stringify(out));
    """)
    assert result["initialDefault"] == WEEK
    assert result["initialGeneral"] == WEEK
    assert result["privateKey"] == "channel:" + "a" * 32
    assert result["privateEffective"] == HOUR
    assert result["expiredOld"] is True
    assert result["keptNew"] is False
    assert result["generalAfterDefault"] == DAY
    assert result["dmAfterDefault"] == DAY
    assert result["clearedBody"] == {"chat": "channel:" + "a" * 32, "retentionMs": None}
    assert result["privateAfterClear"] == DAY
    assert result["cached"] == DAY
    assert result["reloadedDefault"] == DAY
    assert result["changes"] == 3
    assert result["clampLow"] == HOUR
    assert result["clampHigh"] == WEEK
    assert result["clampBad"] == 0
    assert result["labels"] == ["7 days", "3 days", "1 day", "8 hours", "1 hour"]
    assert result["presetMs"][0] == WEEK
    assert result["corrupt"] == {"defaultMs": WEEK, "chats": {"#b": DAY}, "updatedAt": 0}


def test_full_chat_filters_prunes_and_offers_the_picker():
    assert 'from "./chat-retention.js"' in CHAT
    assert "if (chatRetention.isExpired(entry.ts, channelKey)) return;" in CHAT
    assert "setInterval(pruneExpiredMessages, 60000);" in CHAT
    assert "renderRetentionControl();" in CHAT
    assert 'id="chat-retention-select"' in CHAT_HTML
    assert 'aria-label="Messages disappear after"' in CHAT_HTML


def test_dashboard_chat_honours_the_same_preference():
    assert 'import("./chat-retention.js")' in DASHBOARD_CHAT
    assert "if (chatRetention?.isExpired(entry.ts, CHANNEL)) return;" in DASHBOARD_CHAT
    assert "setInterval(pruneExpiredChat, 60000);" in DASHBOARD_CHAT


def test_world_serves_the_retention_module_from_the_app():
    for path in (
        REPO / "world" / "worker.js",
        REPO / "world" / "wrangler.toml",
        REPO / "deploy" / "core" / "caddy_routes.py",
    ):
        assert '"/chat-retention.js",' in path.read_text(encoding="utf-8")

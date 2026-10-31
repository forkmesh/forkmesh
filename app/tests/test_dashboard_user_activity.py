import json
import shutil
import subprocess
from pathlib import Path

import pytest


JS = Path(__file__).resolve().parents[1] / "public" / "dashboard" / "js"
CONTENT = (JS / "06-repo-content.js").read_text(encoding="utf-8")
BOOT = (JS / "10-boot.js").read_text(encoding="utf-8")


def _activity_source():
    start = CONTENT.index("  const USER_ACTIVITY_SLOTS = 288;")
    end = CONTENT.index("  function orgAgentMergeStatsText(")
    return CONTENT[start:end]


def _run(body):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    script = """
const storage = new Map();
globalThis.localStorage = {
  getItem: (key) => storage.has(key) ? storage.get(key) : null,
  setItem: (key, value) => storage.set(key, String(value)),
};
const calls = [];
let reply = null;
globalThis.fetch = async (path, init) => {
  const sent = JSON.parse(init.body);
  calls.push({ path, method: init.method, headers: init.headers, body: sent });
  const answer = reply ? reply(sent) : { status: 200, body: { ok: true, nodeName: "alice", others: new Array(288).fill(0), sources: [] } };
  return { ok: answer.status < 300, status: answer.status, json: async () => answer.body };
};
const state = { session: { nodeName: "alice", sessionToken: "tok" } };
const sessionHeaders = (json) => ({ accept: "application/json", ...(json ? { "content-type": "application/json" } : {}), authorization: "Bearer " + state.session.sessionToken });
const escapeHtml = (value) => String(value).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
const formatCount = (value) => String(value);
const readDashboardTheme = () => "dark";
const seasonActive = () => false;
const $$ = () => [];
globalThis.setTimeout = () => 0;
%s
(async () => {
%s
})().then((result) => console.log(JSON.stringify(result)), (error) => console.log(JSON.stringify({ error: String(error && error.stack || error) })));
""" % (_activity_source(), body)
    out = subprocess.run([node, "-e", script], check=True, capture_output=True,
                         text=True, timeout=30)
    result = json.loads(out.stdout.strip().splitlines()[-1])
    assert "error" not in result, result.get("error")
    return result


def test_boot_starts_counting():
    assert "startUserActivity();" in BOOT
    assert "${branchFacts.merged ? renderUserActivityCard() : \"\"}" in CONTENT


def test_records_slots_for_the_local_day_and_keeps_a_device_key():
    result = _run("""
  const at = new Date(2026, 9, 6, 9, 7);
  recordUserActivity(at);
  recordUserActivity(at);
  recordUserActivity(new Date(2026, 9, 6, 23, 59));
  const day = userActivityDay(at);
  const stored = userActivityStored();
  const device = userActivityDevice();
  return { day, revision: stored.revision, slot: stored.counts[9 * 12 + 1], last: stored.counts[287],
           device, again: userActivityDevice(), next: (recordUserActivity(new Date(2026, 9, 7, 0, 1)), userActivityStored()) };
""")
    assert result["day"] == "2026-10-06"
    assert result["revision"] == 3
    assert result["slot"] == 2
    assert result["last"] == 1
    assert 8 <= len(result["device"]) <= 64
    assert result["again"] == result["device"]
    assert result["next"]["day"] == "2026-10-07"
    assert sum(result["next"]["counts"]) == 1


def test_sync_sends_counts_then_adds_other_devices_to_the_card():
    result = _run("""
  recordUserActivity();
  const slot = userActivitySlot();
  reply = () => {
    const others = new Array(288).fill(0);
    others[slot] = 4;
    return { status: 200, body: { ok: true, nodeName: "alice", others,
      sources: [{ kind: "desktop", total: 3, self: false }, { kind: "ios", total: 1, self: false }, { kind: "web", total: 1, self: true }] } };
  };
  const first = await syncUserActivity(false);
  const idle = await syncUserActivity(false);
  const forcedTooSoon = await syncUserActivity(true);
  recordUserActivity();
  const fresh = await syncUserActivity(false);
  const card = renderUserActivityCard();
  return { first, idle, forcedTooSoon, fresh, calls, merged: userActivityCounts()[slot],
           local: userActivityLocalCounts()[slot], kinds: userActivityOtherKinds(), card };
""")
    assert result["first"] is True
    assert result["idle"] is False
    assert result["forcedTooSoon"] is False
    assert result["fresh"] is True
    first, second = result["calls"]
    assert first["path"] == "/api/accounts/activity-today"
    assert first["method"] == "POST"
    assert first["headers"]["authorization"] == "Bearer tok"
    assert first["body"]["kind"] == "web"
    assert len(first["body"]["counts"]) == 288
    assert sum(first["body"]["counts"]) == 1
    assert sum(second["body"]["counts"]) == 2
    assert result["local"] == 2
    assert result["merged"] == 6
    assert result["kinds"] == ["desktop", "ios"]
    assert "On this browser, desktop and iOS device" in result["card"]
    assert "6 interactions" in result["card"]


def test_another_accounts_answer_or_a_sign_out_drops_other_devices():
    result = _run("""
  reply = () => ({ status: 200, body: { ok: true, nodeName: "bob", others: new Array(288).fill(1), sources: [] } });
  const wrong = await syncUserActivity(true);
  const afterWrong = userActivityCounts().reduce((a, b) => a + b, 0);
  reply = null;
  const ok = await syncUserActivity(true);
  userActivity.others = new Array(288).fill(1);
  userActivity.othersDay = userActivityDay();
  state.session = null;
  const signedOut = await syncUserActivity(true);
  return { wrong, afterWrong, ok, signedOut, afterSignOut: userActivityCounts().reduce((a, b) => a + b, 0), calls: calls.length };
""")
    assert result == {"wrong": False, "afterWrong": 0, "ok": True,
                      "signedOut": False, "afterSignOut": 0, "calls": 2}


def test_sources_text_matches_the_apps():
    result = _run("""
  return [userActivitySourcesText("web", []), userActivitySourcesText("web", ["desktop", "android", "desktop"]),
          userActivitySourcesText("web", ["web"])];
""")
    assert result == ["this browser", "this browser, desktop and Android device",
                      "this browser and another browser"]

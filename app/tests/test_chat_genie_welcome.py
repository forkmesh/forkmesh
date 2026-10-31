#!/usr/bin/env python3

import json
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
CHAT_HTML = (PUBLIC / "chat.html").read_text(encoding="utf-8")
CHAT_CSS = (PUBLIC / "chat.css").read_text(encoding="utf-8")


def _run(script):
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
        cwd=PUBLIC,
    )
    return json.loads(result.stdout)


def _function_source(name):
    match = re.search(
        r"^(?:async )?function " + re.escape(name) + r"\(.*?^}\n", CHAT,
        re.S | re.M)
    assert match, name
    return match.group(0)


MOCK_CONTEXT = """
const calls = { fill: 0, stroke: 0, paint: [] };
const gradient = { addColorStop() {} };
const ctx = new Proxy({}, {
  get(target, prop) {
    if (prop in target) return target[prop];
    if (prop === "createRadialGradient" || prop === "createLinearGradient") return () => gradient;
    if (prop === "measureText") return (text) => ({ width: String(text).length * 10 });
    if (prop === "fill" || prop === "fillRect" || prop === "fillText") return () => { calls.fill += 1; };
    if (prop === "stroke") return () => { calls.stroke += 1; };
    return () => {};
  },
  set(target, prop, value) {
    target[prop] = value;
    return true;
  },
});
const paint = (_ctx, pose) => calls.paint.push(pose);
"""


def test_ten_distinct_welcome_animations_with_plain_text_fallbacks():
    result = _run("""
      import { GENIE_WELCOMES } from "./chat-genie-welcome.js";
      console.log(JSON.stringify(GENIE_WELCOMES.map((w) => ({
        id: w.id, text: w.text, scene: typeof w.scene,
      }))));
    """)
    assert len(result) == 10
    assert len({w["id"] for w in result}) == 10
    assert len({w["text"] for w in result}) == 10
    for welcome in result:
        assert welcome["scene"] == "function"
        assert "genie spider" in welcome["text"].lower()
        assert "welcome" in welcome["text"].lower()


def test_index_accepts_only_known_animations():
    result = _run("""
      import { genieWelcomeIndex } from "./chat-genie-welcome.js";
      console.log(JSON.stringify(
        [0, 9, 10, -1, 2.5, "3", null, undefined, true].map((v) => genieWelcomeIndex(v))));
    """)
    assert result == [0, 9, None, None, None, None, None, None, None]


def test_pick_is_random_over_all_ten_and_never_repeats_the_last():
    result = _run("""
      import { pickGenieWelcome } from "./chat-genie-welcome.js";
      const fresh = new Set();
      for (let i = 0; i < 10; i += 1) fresh.add(pickGenieWelcome(null, () => i / 10));
      fresh.add(pickGenieWelcome(null, () => 0.9999999));
      const repeats = [];
      const reached = {};
      for (let last = 0; last < 10; last += 1) {
        reached[last] = new Set();
        for (let i = 0; i < 9; i += 1) {
          const pick = pickGenieWelcome(last, () => i / 9);
          if (pick === last) repeats.push(last);
          reached[last].add(pick);
        }
        reached[last] = reached[last].size;
      }
      console.log(JSON.stringify({ fresh: [...fresh].sort((a, b) => a - b), repeats, reached }));
    """)
    assert result["fresh"] == list(range(10))
    assert result["repeats"] == []
    assert set(result["reached"].values()) == {9}


def test_every_animation_draws_the_spider_through_its_whole_run():
    result = _run(MOCK_CONTEXT + """
      import { GENIE_WELCOMES, GENIE_WELCOME_SECONDS, drawGenieWelcome } from "./chat-genie-welcome.js";
      const out = [];
      GENIE_WELCOMES.forEach((welcome, index) => {
        let bad = [];
        const before = calls.paint.length;
        for (let t = 0; t <= GENIE_WELCOME_SECONDS + 0.5; t += 0.05) {
          const frame = drawGenieWelcome(ctx, paint, index, t);
          for (const key of ["x", "y"]) {
            if (!Number.isFinite(frame[key])) bad.push([t, key]);
          }
        }
        const last = calls.paint[calls.paint.length - 1];
        out.push({
          id: welcome.id,
          bad,
          painted: calls.paint.length - before,
          endReaction: last.reaction,
          endAir: last.air,
          endMood: last.mood,
        });
      });
      console.log(JSON.stringify(out));
    """)
    for welcome in result:
        assert welcome["bad"] == [], welcome
        assert welcome["painted"] > 40, welcome
        assert welcome["endReaction"] == "", welcome
        assert welcome["endAir"] == 0, welcome
        assert welcome["endMood"] == "idle", welcome


def test_draw_without_the_painter_still_draws_the_stage():
    result = _run(MOCK_CONTEXT + """
      import { drawGenieWelcome } from "./chat-genie-welcome.js";
      drawGenieWelcome(ctx, null, 1, 1);
      console.log(JSON.stringify({ fill: calls.fill, painted: calls.paint.length }));
    """)
    assert result["fill"] > 0
    assert result["painted"] == 0


def test_painter_loads_genie_spider_script_without_the_wandering_spider():
    result = _run("""
      import { loadGeniePainter, GENIE_WELCOME_SCRIPT } from "./chat-genie-welcome.js";
      const appended = [];
      const html = { dataset: {} };
      const win = {
        document: {
          documentElement: html,
          head: { append(node) { appended.push(node); } },
          createElement() {
            const listeners = {};
            return { listeners, addEventListener(type, fn) { listeners[type] = fn; } };
          },
        },
      };
      const pending = loadGeniePainter(win);
      const again = loadGeniePainter(win);
      win.ForkMeshGenie = { paint() {} };
      appended[0].listeners.load();
      const paint = await pending;
      console.log(JSON.stringify({
        same: pending === again,
        scripts: appended.map((node) => node.src),
        genie: html.dataset.genie,
        loaded: paint === win.ForkMeshGenie.paint,
        expected: GENIE_WELCOME_SCRIPT,
      }));
    """)
    assert result["same"] is True
    assert result["scripts"] == [result["expected"]]
    assert result["expected"] == "/genie-spider.js"
    assert result["genie"] == "off"
    assert result["loaded"] is True


def test_welcome_button_lives_in_the_channel_header():
    button = re.search(r'<button\s+id="chat-genie-welcome".*?</button>', CHAT_HTML, re.S)
    assert button
    assert "hidden" in button.group(0)
    assert 'data-instance-plugin="genie"' in button.group(0)
    assert "Welcome" in button.group(0)
    heading = CHAT_HTML.index('class="chat-channel-heading"')
    controls = CHAT_HTML.index('class="chat-main-controls"')
    assert heading < button.start() < controls
    assert ".chat-genie-welcome-button:not([hidden])" in CHAT_CSS


def test_button_shows_only_in_welcome_channel_for_members():
    result = _run(
        "\n".join(_function_source(name) for name in (
            "isPrivateChannelKey",
            "privateChannelKey",
            "privateChannelForKey",
            "isEveryoneChannel",
            "isWelcomeChannel",
        ))
        + """
      const WELCOME_CHANNEL_NAME = "welcome";
      const [w, g, c] = ["a", "b", "c"].map((ch) => ch.repeat(32));
      const privateChannels = new Map([
        [w, { id: w, kind: "everyone", name: "welcome" }],
        [g, { id: g, kind: "everyone", name: "general" }],
        [c, { id: c, kind: "channel", name: "welcome" }],
      ]);
      console.log(JSON.stringify([w, g, c].map(privateChannelKey).concat("#general")
        .map((key) => isWelcomeChannel(key))));
    """)
    assert result == [True, False, False, False]
    heading = _function_source("updateChannelHeading")
    assert "genieWelcomeBtn.hidden = !isWelcomeChannel() || !canJoinChannel()" in heading


def test_send_posts_a_chat_frame_carrying_the_animation():
    source = _function_source("sendGenieWelcome")
    assert "pickGenieWelcome(lastGenieWelcome)" in source
    assert "genieWelcome: index" in source
    assert "GENIE_WELCOMES[index].text" in source
    assert "isWelcomeChannel()" in source
    assert "GENIE_WELCOME_COOLDOWN_MS" in source
    assert 'makePlain("chat"' in source
    assert "genieWelcomeBtn?.addEventListener(\"click\", sendGenieWelcome)" in CHAT


def test_received_frames_keep_the_animation_on_the_record():
    entry = _function_source("renderChatEntry")
    assert "viewOnce ? null : entry.genieWelcome" in entry
    append = _function_source("appendMessage")
    assert "genieWelcome: genieWelcomeIndex(genieWelcome)" in append
    row = _function_source("buildRow")
    assert "record.genieWelcome !== null && !record.deleted" in row
    render = _function_source("renderGenieWelcome")
    assert "playedGenieWelcomes" in render
    assert "GENIE_WELCOME_AUTOPLAY_MS" in render

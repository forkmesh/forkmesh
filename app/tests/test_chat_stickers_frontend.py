#!/usr/bin/env python3

import json
import re
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
REPO = ROOT.parent
SPEC = (REPO / "docs" / "app" / "chat-stickers.md").read_text(encoding="utf-8")
CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
DASHBOARD = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
CHAT_CSS = (PUBLIC / "chat.css").read_text(encoding="utf-8")
WORLD_CSS = (REPO / "world" / "public" / "world" / "world.css").read_text(
    encoding="utf-8")

CLIENTS = {
    "chat": (CHAT, ""),
    "dashboard": (DASHBOARD, "  "),
}

FAKE_DOM = r"""
class El {
  constructor(tag) {
    this.tagName = tag.toUpperCase();
    this.children = [];
    this.dataset = {};
    this.attrs = {};
    this.listeners = {};
    this.style = { props: {}, setProperty(key, value) { this.props[key] = value; } };
    this.classes = new Set();
    this.text = "";
  }
  get className() { return [...this.classes].join(" "); }
  set className(value) {
    this.classes = new Set(String(value).split(/\s+/).filter(Boolean));
  }
  get classList() {
    const classes = this.classes;
    return {
      toggle(name, force) {
        const on = force === undefined ? !classes.has(name) : Boolean(force);
        if (on) classes.add(name); else classes.delete(name);
        return on;
      },
      contains: (name) => classes.has(name),
      add: (name) => classes.add(name),
    };
  }
  set textContent(value) { this.children = []; this.text = String(value); }
  get textContent() {
    return this.text + this.children.map((child) => child.textContent).join("");
  }
  append(...nodes) { this.children.push(...nodes); }
  setAttribute(key, value) { this.attrs[key] = String(value); }
  addEventListener(type, handler) { (this.listeners[type] ||= []).push(handler); }
  click() {
    for (const handler of this.listeners.click || []) handler({ stopPropagation() {} });
  }
}
const document = { createElement: (tag) => new El(tag) };
"""


def _const_source(source, name):
    match = re.search(
        r"^ *const " + re.escape(name) + r" = .*?;\n", source, re.S | re.M)
    assert match, name
    return match.group(0)


def _function_source(source, indent, name):
    match = re.search(
        r"^" + indent + r"function " + re.escape(name) + r"\(.*?^" + indent
        + r"}\n", source, re.S | re.M)
    assert match, name
    return match.group(0)


def _dedent(text, indent):
    if not indent:
        return text
    return "\n".join(
        line[len(indent):] if line.startswith(indent) else line
        for line in text.split("\n"))


def _catalog(source):
    script = (
        _const_source(source, "STICKER_CATALOG")
        + "console.log(JSON.stringify(STICKER_CATALOG));")
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True)
    return json.loads(result.stdout)


def _spec_catalog():
    rows = []
    for line in SPEC.splitlines():
        match = re.match(
            r"\| `([a-z-]+)` \| (.+?) \| `(#[0-9a-f]{6})` \| (-?\d+)° \| \S+ (.+?) \|$",
            line)
        if not match:
            continue
        slug, text, color, tilt, codes = match.groups()
        emoji = "".join(chr(int(code[2:], 16)) for code in codes.split())
        rows.append({
            "slug": slug, "text": text, "color": color, "tilt": int(tilt),
            "emoji": emoji,
        })
    assert len(rows) == 8
    return rows


@pytest.mark.parametrize("client", sorted(CLIENTS))
def test_catalog_matches_the_spec_table_in_order(client):
    source, _ = CLIENTS[client]
    assert _catalog(source) == _spec_catalog()


def _scenario(client):
    source, indent = CLIENTS[client]
    names = [
        "reactionKey", "applyReaction", "applySticker", "stickerBadge",
        "renderStickers", "toggleSticker",
    ]
    helpers = "\n".join(
        _dedent(_function_source(source, indent, name), indent)
        for name in names)
    consts = "\n".join(
        _dedent(_const_source(source, name), indent)
        for name in ("STICKER_CATALOG", "STICKERS_BY_SLUG"))
    script = FAKE_DOM + consts + """
      const MAX_NAME = 32;
      const CHANNEL_LABEL = "#general";
      const activeChannel = "#general";
      const selfId = "me";
      const rows = new Map();
      const threadRenderedRows = new Map();
      const threads = { target: () => null };
      const reactions = new Map();
      const stickers = new Map();
      const seen = new Set();
      const sent = [];
      const canJoinChannel = () => true;
      const canJoinChat = () => true;
      const lockChatForNonUser = () => {};
      const showUserOnlyState = () => {};
      const channelWireLabel = (channel) => channel;
      const displayName = () => "Me";
      let nextId = 0;
      const makePlain = (type, extra) => ({ type, id: `p${++nextId}`, ...extra });
      const runWhenConnected = (fn) => fn();
      const send = (plain) => sent.push(plain);
      const renderReactions = () => {};
      const FRAME_AUTH = "frameAuth";
      const frameAuth = { frameAuth: (plain) => plain?.frameAuth || null };
      const sealedBy = (node) => ({ frameAuth: { sealed: true, node, acct: node, kid: "k".repeat(16), self: false } });
    """ + helpers + """
      const record = {
        id: "m1", channel: "#general", el: new El("div"),
        stickersEl: new El("div"),
      };
      rows.set("m1", record);
      const frame = (extra) => ({ target: "m1", added: true, ...extra });
      applyReaction(frame({ emoji: "\\u{1F389}", reactorId: "p1", sender: "Pat", reactorName: "Forged", ...sealedBy("p1") }));
      applyReaction(frame({ emoji: "\\u{1F389}", sticker: "party", reactorId: "p1", sender: "Pat", reactorName: "Forged", ...sealedBy("p1") }));
      applyReaction(frame({ emoji: "\\u2705", sticker: "lgtm", reactorId: "p2", sender: "Ana", reactorName: "Mallory", ...sealedBy("p2") }));
      applyReaction(frame({ emoji: "\\u2705", sticker: "lgtm", reactorId: "me", reactorName: "Me" }));
      applyReaction(frame({ emoji: "\\u{1F984}", sticker: "unicorn-from-the-future", reactorId: "p3", reactorName: "Kim" }));
      const badges = () => record.stickersEl.children.map((badge) => ({
        slug: badge.dataset.sticker,
        text: badge.textContent,
        title: badge.title,
        mine: badge.classList.contains("is-mine"),
        background: badge.style.background,
        tilt: badge.style.props["--chat-sticker-tilt"],
      }));
      const placed = badges();
      const reactionKeys = [...reactions.get("m1").keys()];
      const stickerKeys = [...stickers.get("m1").keys()];
      const hasStickers = record.el.classList.contains("has-stickers");
      applyReaction(frame({ emoji: "\\u{1F389}", sticker: "party", reactorId: "p1", added: false }));
      const partyRemovedKeepsReaction = reactions.get("m1").has("\\u{1F389}") &&
        !stickers.get("m1").has("party");
      toggleSticker("m1", "lgtm");
      const afterUnstick = badges();
      record.stickersEl.children[0].click();
      const afterClick = badges();
      applyReaction(frame({ emoji: "\\u2705", sticker: "lgtm", reactorId: "p2", added: false }));
      applyReaction(frame({ emoji: "\\u2705", sticker: "lgtm", reactorId: "me", added: false }));
      console.log(JSON.stringify({
        placed, reactionKeys, stickerKeys, hasStickers, partyRemovedKeepsReaction,
        afterUnstick, afterClick, sent,
        clearedClass: record.el.classList.contains("has-stickers"),
        cleared: stickers.has("m1"),
        seen: [...seen],
      }));
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True, cwd=PUBLIC)
    return json.loads(result.stdout)


@pytest.mark.parametrize("client", sorted(CLIENTS))
def test_stickers_are_keyed_apart_from_reactions_and_render_in_catalog_order(client):
    result = _scenario(client)
    assert result["reactionKeys"] == ["\U0001F389", "\U0001F984"]
    assert result["stickerKeys"] == ["party", "lgtm"]
    assert result["hasStickers"] is True
    assert [badge["slug"] for badge in result["placed"]] == ["lgtm", "party"]
    lgtm, party = result["placed"]
    assert lgtm["text"] == "LGTM×2"
    assert lgtm["title"] == "LGTM from Ana, Me"
    assert lgtm["mine"] is True
    assert lgtm["background"] == "#8250df"
    assert lgtm["tilt"] == "-6deg"
    assert party["text"] == "Party!"
    assert party["mine"] is False
    assert party["tilt"] == "7deg"
    assert result["partyRemovedKeepsReaction"] is True


@pytest.mark.parametrize("client", sorted(CLIENTS))
def test_toggling_a_sticker_sends_a_reaction_frame_with_the_fallback(client):
    result = _scenario(client)
    first, second = result["sent"]
    assert first == {
        "type": "reaction", "id": first["id"], "conversation": "#general",
        "target": "m1", "emoji": "✅", "sticker": "lgtm",
        "reactorId": "me", "reactorName": "Me", "added": False,
    }
    assert second["added"] is True
    assert second["sticker"] == "lgtm"
    assert result["seen"] == [first["id"], second["id"]]
    assert result["afterUnstick"][0]["text"] == "LGTM"
    assert result["afterUnstick"][0]["mine"] is False
    assert result["afterClick"][0]["text"] == "LGTM×2"
    assert result["afterClick"][0]["mine"] is True
    assert result["cleared"] is False
    assert result["clearedClass"] is False


def test_web_chat_offers_add_sticker_next_to_add_reaction():
    pattern = re.compile(
        r'"Add reaction",\s*"React",\s*\(button\) => showEmojiPicker\(button, record\.id\),\s*\)\);\s*'
        r'actions\.append\(messageAction\(\s*"Add sticker",\s*"Sticker",\s*'
        r'\(button\) => showStickerPicker\(button, record\.id\),')
    assert len(pattern.findall(CHAT)) == 2
    assert 'Sticker: "sticker"' in CHAT
    assert CHAT.count("stickers.delete(") == CHAT.count("reactions.delete(") + 1


def test_dashboard_chat_offers_add_sticker_only_on_forkmesh_messages():
    actions = DASHBOARD[DASHBOARD.index("function buildMessageActions("):
                        DASHBOARD.index("let activeReactionPicker = null;")]
    assert actions.index('ariaLabel: "Add reaction"') < actions.index(
        'ariaLabel: "Add sticker"')
    assert "if (record.id && !record.external) {" in DASHBOARD
    picker = _function_source(DASHBOARD, "  ", "showStickerPicker")
    assert "record.external" in picker
    assert "for (const sticker of STICKER_CATALOG)" in _function_source(
        DASHBOARD, "  ", "renderStickers")
    for name in ("reactions.delete(", "reactions.clear("):
        assert DASHBOARD.count(name) == DASHBOARD.count(
            name.replace("reactions", "stickers"))


def test_sticker_styles_ship_with_every_web_surface():
    for source in (CHAT_CSS, WORLD_CSS):
        assert ".chat-stickers" in source
        assert ".chat-sticker-picker" in source
        assert "rotate(var(--chat-sticker-tilt, 0deg))" in source
    assert ".chat-msg.has-stickers .chat-message-actions" in CHAT_CSS
    assert ".chat-message-row.has-stickers" in WORLD_CSS

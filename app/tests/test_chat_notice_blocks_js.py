import json
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "public" / "chat-apps.js"
STYLES = ROOT / "public" / "chat.css"

HARNESS = r'''
const { readFileSync } = require("node:fs");

class Node {
  constructor(tag) {
    this.tag = tag;
    this.className = "";
    this.own = "";
    this.children = [];
  }
  set textContent(value) {
    this.own = String(value);
  }
  get textContent() {
    return this.own + this.children.map((child) => child.textContent).join("");
  }
  append(...kids) {
    this.children.push(...kids);
  }
  addEventListener() {}
}

globalThis.document = { createElement: (tag) => new Node(tag) };
globalThis.window = {};
globalThis.localStorage = { getItem: () => null };
globalThis.fetch = () => Promise.reject(new Error("offline"));

new Function(readFileSync(process.argv[2], "utf8"))();

function flatten(node, out) {
  out.push({ tag: node.tag, className: node.className, text: node.own });
  for (const child of node.children) flatten(child, out);
  return out;
}

const message = {
  notice: true,
  blocks: [
    {
      type: "section",
      text: "🪪 Verification · ✅ accepted · 2026-09-28 13:00:00 UTC",
      fields: ["📨 To: jo@example.org",
               "📄 Body: Hi jo, confirm your address: [link]",
               "⚠️ Reason: HTTP 403: nope",
               "Reason: HTTP 403: nope",
               "no label here"],
    },
    {
      type: "section",
      text: "🗂️ Task activity · 👀 opened · 1970-01-01",
      fields: ["🆔 Message ID: m-1"],
    },
    {
      type: "section",
      text: "🔑 Password reset · ⛔ REJECTED · 1970-01-01",
      fields: ["📊 Budget: 150 of 150 sent today (UTC)"],
    },
  ],
};

const notice = new Node("div");
window.ForkMeshChatApps.renderBlocks(notice, message, null);
const plain = new Node("div");
window.ForkMeshChatApps.renderBlocks(plain, { blocks: message.blocks }, null);
console.log(JSON.stringify({ notice: flatten(notice, []), plain: flatten(plain, []) }));
'''


def _nodes(payload, class_name):
    return [node for node in payload
            if class_name in node["className"].split()]


@pytest.fixture(scope="module")
def rendered(tmp_path_factory):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    script = tmp_path_factory.mktemp("chat-apps") / "harness.cjs"
    script.write_text(HARNESS, encoding="utf-8")
    result = subprocess.run(
        [node, str(script), str(MODULE)], capture_output=True, text=True,
        timeout=60)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def test_a_notice_headline_colours_its_result_and_mutes_the_rest(rendered):
    titles = _nodes(rendered["notice"], "chat-notice-title")
    assert [node["text"] for node in titles] == [
        "🪪 Verification", "🗂️ Task activity",
        "🔑 Password reset"]
    parts = [(node["text"], node["className"])
             for node in _nodes(rendered["notice"], "chat-notice-part")]
    assert parts == [
        ("✅ accepted", "chat-notice-part is-good"),
        ("2026-09-28 13:00:00 UTC", "chat-notice-part is-muted"),
        ("👀 opened", "chat-notice-part is-info"),
        ("1970-01-01", "chat-notice-part is-muted"),
        ("⛔ REJECTED", "chat-notice-part is-bad"),
        ("1970-01-01", "chat-notice-part is-muted")]


def test_a_notice_field_splits_into_an_emoji_label_and_its_value(rendered):
    labels = [node["text"]
              for node in _nodes(rendered["notice"], "chat-notice-label")]
    values = [node["text"]
              for node in _nodes(rendered["notice"], "chat-notice-value")]
    assert labels == [
        "📨 To", "📄 Body", "⚠️ Reason", "Reason",
        "🆔 Message ID", "📊 Budget"]
    assert values[:4] == ["jo@example.org",
                          "Hi jo, confirm your address: [link]",
                          "HTTP 403: nope", "HTTP 403: nope"]
    unlabelled = [node for node in _nodes(rendered["notice"], "chat-app-field")
                  if "chat-notice-row" not in node["className"].split()]
    assert [node["text"] for node in unlabelled] == ["no label here"]


def test_an_app_message_keeps_the_plain_field_layout(rendered):
    assert not _nodes(rendered["plain"], "chat-notice-card")
    assert not _nodes(rendered["plain"], "chat-notice-label")
    fields = _nodes(rendered["plain"], "chat-app-field")
    assert [node["text"] for node in fields][:1] == [
        "📨 To: jo@example.org"]


def test_the_tone_classes_all_have_a_colour():
    styles = STYLES.read_text(encoding="utf-8")
    for tone in ("good", "warn", "bad", "info"):
        assert ".chat-notice-part.is-%s { color: var(--tone-%s); }" % (
            tone, tone) in styles
        assert "--tone-%s:" % tone in styles

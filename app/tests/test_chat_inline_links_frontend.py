#!/usr/bin/env python3
"""Links live inside chat text on every surface (issue #539).

A URL belongs in the sentence that explains it. Before this, only the
standalone chat page resolved one: the dashboard chat and the desktop client
rendered a pasted address as dead characters, so the only way to hand someone
something clickable was to send the URL as a message of its own.

These contracts pin the shared rules across the three renderers:

* the two shapes people write — a pasted http(s) URL and markdown's
  ``[label](url)`` — both become links;
* trailing sentence punctuation stays outside the address, while a closing
  parenthesis the address itself opened stays in it;
* only http/https/mailto targets are links, and a rejected target degrades to
  the words the author wrote rather than to a dead anchor;
* the desktop keeps the address a web-composed message carries in its rich
  source, which its flattened ``text`` fallback throws away.
"""

import json
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
PUBLIC = ROOT / "app" / "public"
RICH_TEXT = PUBLIC / "chat-rich-text.js"
DASHBOARD_CHAT = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
SHELL = (PUBLIC / "dashboard" / "shell.html").read_text(encoding="utf-8")
DESKTOP = ROOT / "desktop" / "src"
LINK_TEXT = (DESKTOP / "ChatLinkText.h").read_text(encoding="utf-8")
CHAT_FORMAT = (DESKTOP / "ChatFormat.h").read_text(encoding="utf-8")
MESSAGE_ROW = (DESKTOP / "MessageRow.cpp").read_text(encoding="utf-8")
SERVER_NODE = (DESKTOP / "ServerNode.cpp").read_text(encoding="utf-8")
COMPOSER = (DESKTOP / "MainWindowIssues.cpp").read_text(encoding="utf-8")
MESSAGES = (DESKTOP / "MainWindowMessages.cpp").read_text(encoding="utf-8")

# A DOM small enough to run a renderer against, and a walker that reports what
# it built as "text" runs and "A[href]{label}" links.
FAKE_DOM = """
  class FakeNode {
    constructor(tag = "#text", text = "") {
      this.tagName = tag;
      this.text = text;
      this.className = "";
      this.href = "";
      this.rel = "";
      this.target = "";
      this.childNodes = [];
    }
    set textContent(value) {
      this.childNodes = [];
      this.text = String(value);
    }
    get textContent() { return this.text; }
    append(...children) { this.childNodes.push(...children); }
    replaceChildren(...children) { this.childNodes = [...children]; }
    addEventListener() {}
  }
  globalThis.Node = FakeNode;
  globalThis.window = { location: { href: "https://app.forkmesh.com/dashboard/chat" } };
  globalThis.document = {
    createElement: (tag) => new FakeNode(tag),
    createTextNode: (text) => new FakeNode("#text", String(text)),
  };
  const textOf = (node) =>
    (node.text || "") + node.childNodes.map(textOf).join("");
  // Adjacent plain runs are merged: where one renderer splits a mention into
  // its own text node and the other does not is a difference in node counts,
  // not in what the reader sees.
  const describe = (node) => {
    const out = [];
    const walk = (current) => {
      if (current.tagName === "a") {
        out.push("A[" + current.href + "]{" + textOf(current) + "}");
        return;
      }
      if (current.text) {
        if (out.length && !out[out.length - 1].startsWith("A[")) {
          out[out.length - 1] += current.text;
        } else {
          out.push(current.text);
        }
      }
      current.childNodes.forEach(walk);
    };
    node.childNodes.forEach(walk);
    return out;
  };
"""

CASES = [
    "status is at https://forkmesh.com/status, see it.",
    "(see https://en.wikipedia.test/wiki/Fork_(software))",
    "read the [release notes](https://forkmesh.com/blog) now",
    "[click me](javascript:alert(1)) stays text",
    "plain @jett with no link",
]


def _run_module(script):
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def _region(source, start, end):
    return source[source.index(start):source.index(end)]


def _standalone_chat_render():
    script = f"""
      {FAKE_DOM}
      const {{ renderRichText }} = await import({json.dumps(RICH_TEXT.as_uri())});
      const out = {{}};
      for (const source of {json.dumps(CASES)}) {{
        const root = new FakeNode("root");
        renderRichText(root, {{ text: source }});
        out[source] = describe(root);
      }}
      console.log(JSON.stringify(out));
    """
    return _run_module(script)


def _dashboard_chat_render():
    # dashboard-chat.js is a classic script inside one IIFE, so its renderer is
    # lifted out by source region and run against the same fake DOM. Mentions
    # are somebody else's contract here: the stub only records that non-link
    # runs were handed over intact.
    helpers = _region(
        DASHBOARD_CHAT,
        "  const SAFE_CHAT_LINK_PROTOCOLS",
        "  // ---- @mention autocomplete",
    )
    script = f"""
      {FAKE_DOM}
      const appendMentionText = (container, text) => {{
        if (text) container.append(document.createTextNode(text));
      }};
      {helpers}
      const out = {{}};
      for (const source of {json.dumps(CASES)}) {{
        const root = new FakeNode("root");
        renderMessageText(root, source);
        out[source] = describe(root);
      }}
      console.log(JSON.stringify(out));
    """
    return _run_module(script)


def test_both_web_renderers_link_the_same_text_the_same_way():
    standalone = _standalone_chat_render()
    dashboard = _dashboard_chat_render()
    assert standalone == dashboard

    rendered = dashboard
    assert rendered[CASES[0]] == [
        "status is at ",
        "A[https://forkmesh.com/status]{https://forkmesh.com/status}",
        ", see it.",
    ]
    assert rendered[CASES[1]] == [
        "(see ",
        "A[https://en.wikipedia.test/wiki/Fork_(software)]"
        "{https://en.wikipedia.test/wiki/Fork_(software)}",
        ")",
    ]
    assert rendered[CASES[2]] == [
        "read the ",
        "A[https://forkmesh.com/blog]{release notes}",
        " now",
    ]
    # A target the browser must not be handed is not a link, and the label it
    # carried is still readable.
    assert not any(run.startswith("A[") for run in rendered[CASES[3]])
    assert "".join(rendered[CASES[3]]) == "click me stays text"
    assert "".join(rendered[CASES[4]]) == CASES[4]


def test_dashboard_links_are_safe_and_styled():
    helpers = _region(
        DASHBOARD_CHAT,
        "  const SAFE_CHAT_LINK_PROTOCOLS",
        "  // ---- @mention autocomplete",
    )
    assert 'new Set(["http:", "https:", "mailto:"])' in helpers
    assert 'anchor.rel = "noopener noreferrer";' in helpers
    assert 'anchor.target = "_blank";' in helpers
    assert 'anchor.textContent = label;' in helpers  # never innerHTML
    assert ".chat-link {" in SHELL
    assert "renderMessageText(record.textEl, text)" in DASHBOARD_CHAT


def test_desktop_renders_the_same_two_shapes_in_message_bodies():
    assert "renderBodyHtml" in LINK_TEXT
    # Only schemes that cannot execute anything reach the browser.
    assert 'scheme == QLatin1String("mailto")' in LINK_TEXT
    assert 'scheme != QLatin1String("http") && scheme != QLatin1String("https")' in LINK_TEXT
    # Bodies stay escaped: every run goes through toHtmlEscaped().
    assert "QString escaped = segment.toHtmlEscaped();" in LINK_TEXT

    assert '#include "ChatLinkText.h"' in MESSAGE_ROW
    assert "renderMentionedText(message.text, m_mentionProfiles)" in MESSAGE_ROW
    assert "forkmesh::chatformat::toHtml(" in MESSAGE_ROW
    assert "ChatLinkText::sanitizedHref(" in CHAT_FORMAT
    assert "ChatLinkText::rawUrlEnd(text, index)" in CHAT_FORMAT
    # A link tells the reader where it goes before the click, and only a
    # re-sanitized target is handed on to be opened.
    assert "linkTargetTooltip(href)" in MESSAGE_ROW
    assert "ChatLinkText::sanitizedHref(href)" in MESSAGE_ROW
    assert "emit linkActivated(target);" in MESSAGE_ROW
    assert "MessageRow::linkActivated" in MESSAGES
    assert "QDesktopServices::openUrl(target);" in MESSAGES

    # Writing one: the composer offers the same affordance as the web
    # formatting bar instead of requiring the markdown by hand.
    assert "setOcticon(linkButton, \"link\", 18);" in COMPOSER
    assert "&MainWindow::insertLinkIntoComposer" in COMPOSER
    assert "ChatLinkText::linkInsertion(" in MESSAGES


def test_desktop_keeps_the_address_a_web_message_carries():
    # The web sends a rich source plus a flattened "text" that rewrites
    # [label](url) down to "label"; flattening the source here instead is what
    # keeps the address reachable on the desktop.
    assert "QString chatBodyText(const QJsonObject &message, int maxChars)" in SERVER_NODE
    assert "forkmesh::chatformat::bodySource(message, maxChars)" in SERVER_NODE
    assert 'rich.value(QStringLiteral("source")).toString()' in CHAT_FORMAT
    assert SERVER_NODE.count(
        "out.text = filteredChatText(chatBodyText(message, kMaxTextChars));") == 1
    assert 'chatBodyText(message, kMaxTextChars));' in SERVER_NODE
    # An oversized rich source is refused with the rest of the payload, and an
    # edited message drops the source it superseded.
    assert 'message.value("richText").toObject().value("source").toString().size() <=' in SERVER_NODE
    assert 'message.remove("richText");' in SERVER_NODE


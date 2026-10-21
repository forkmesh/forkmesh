import importlib.util
import json
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
PUBLIC = ROOT / "app" / "public"
RICH_TEXT = PUBLIC / "chat-rich-text.js"
CHAT_JS = (PUBLIC / "chat.js").read_text(encoding="utf-8")
CHAT_HTML = (PUBLIC / "chat.html").read_text(encoding="utf-8")
CHAT_CSS = (PUBLIC / "chat.css").read_text(encoding="utf-8")
ENTRY = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
DESKTOP = ROOT / "desktop" / "src"
MESSAGES = (DESKTOP / "MainWindowMessages.cpp").read_text(encoding="utf-8")
ACTIONS = (DESKTOP / "MainWindowActions.cpp").read_text(encoding="utf-8")
ISSUES = (DESKTOP / "MainWindowIssues.cpp").read_text(encoding="utf-8")
BLOCKS = (DESKTOP / "ChatBlocks.h").read_text(encoding="utf-8")
OFFICE = (DESKTOP / "OfficeChannelMirror.cpp").read_text(encoding="utf-8")

FAKE_DOM = """
  class FakeNode {
    constructor(tag = "#text", text = "") {
      this.tagName = tag;
      this.text = text;
      this.href = "";
      this.childNodes = [];
    }
    append(...children) { this.childNodes.push(...children); }
    replaceChildren(...children) { this.childNodes = [...children]; }
  }
  globalThis.Node = FakeNode;
  globalThis.window = { location: { href: "https://app.forkmesh.com/chat" } };
  globalThis.document = {
    createElement: (tag) => new FakeNode(tag),
    createTextNode: (text) => new FakeNode("#text", String(text)),
  };
  const html = (node) => {
    if (node.tagName === "#text") return node.text;
    const inner = node.childNodes.map(html).join("");
    if (node.tagName === "root") return inner;
    const href = node.href ? ` href="${node.href}"` : "";
    return `<${node.tagName}${href}>${inner}</${node.tagName}>`;
  };
"""

CASES = [
    "*bold* and _italic_ and ~strike~ and `code`",
    "old **bold** ~~gone~~ ++under++",
    "snake_case_name and 2*3*4 and file_name.txt",
    "_see *both*_ <https://forkmesh.com|ForkMesh> <mailto:jo@example.com>",
    "[docs](https://forkmesh.com/docs) https://x.test/a_b_c @jett",
    "- one\n- *two*\n1. first\n2) second\n> quoted _text_\nafter",
    "```\nconst a = *b*;\n```\ntail",
    "inline ```block``` here",
    "<javascript:alert(1)|bad> [x](javascript:1) <b>raw</b> & co",
]


def _web(cases):
    script = f"""
      {FAKE_DOM}
      const rich = await import({json.dumps(RICH_TEXT.as_uri())});
      const out = {{}};
      for (const source of {json.dumps(cases)}) {{
        const root = new FakeNode("root");
        rich.renderRichText(root, {{ text: source }});
        out[source] = {{ html: html(root), plain: rich.plainTextFromRichSource(source) }};
      }}
      console.log(JSON.stringify(out));
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True)
    return json.loads(result.stdout)


def _chat_format():
    spec = importlib.util.spec_from_file_location(
        "chat_format", ROOT / "app" / "src" / "chat_format.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_web_renders_slack_style_markers():
    out = _web(CASES)
    assert out[CASES[0]]["html"] == (
        "<strong>bold</strong> and <em>italic</em> and <s>strike</s> and "
        "<code>code</code>")
    assert out[CASES[1]]["html"] == (
        "old <strong>bold</strong> <s>gone</s> <u>under</u>")
    assert out[CASES[2]]["html"] == CASES[2]
    assert out[CASES[3]]["html"] == (
        "<em>see <strong>both</strong></em> "
        "<a href=\"https://forkmesh.com/\">ForkMesh</a> "
        "<a href=\"mailto:jo@example.com\">jo@example.com</a>")
    assert out[CASES[5]]["html"] == (
        "<ul><li>one</li><li><strong>two</strong></li></ul>"
        "<ol><li>first</li><li>second</li></ol>"
        "<blockquote>quoted <em>text</em></blockquote>after")
    assert out[CASES[6]]["html"] == (
        "<pre><code>const a = *b*;</code></pre>tail")
    assert out[CASES[7]]["html"] == "inline <code>block</code> here"
    assert "<a" not in out[CASES[8]]["html"]
    assert "<b>raw</b>" in out[CASES[8]]["html"]


def test_web_plain_fallback_matches_the_server_and_drops_markers():
    out = _web(CASES)
    chat_format = _chat_format()
    for source in CASES:
        assert out[source]["plain"] == chat_format.to_text(
            source, links=False), source
    assert out[CASES[0]]["plain"] == "bold and italic and strike and code"
    assert out[CASES[5]]["plain"] == (
        "\u2022 one\n\u2022 two\n1. first\n2. second\n> quoted text\nafter")


def test_web_composer_writes_slack_markers_with_shortcuts():
    assert 'bold: ["*", "*"]' in CHAT_JS
    assert 'italic: ["_", "_"]' in CHAT_JS
    assert 'strike: ["~", "~"]' in CHAT_JS
    assert 'codeblock: ["```\\n", "\\n```"]' in CHAT_JS
    assert "const formatShortcut = composerFormatShortcut(event);" in CHAT_JS
    assert 'return { b: "bold", i: "italic" }[key] || "";' in CHAT_JS
    assert 'data-format="codeblock"' in CHAT_HTML
    assert 'title="Bold: *text* (Ctrl+B)"' in CHAT_HTML
    assert ".chat-text pre {" in CHAT_CSS


def test_email_html_is_escaped_and_styled_inline():
    chat_format = _chat_format()
    html = chat_format.to_html(
        "*Deploy* done <https://x.test/run|logs>\n- `api` up\n> <b>q</b>")
    assert html.startswith("<strong>Deploy</strong> done <a href=\"https://x.test/run\"")
    assert "<ul style=" in html and "<li><code style=" in html
    assert "<blockquote style=" in html and "&lt;b&gt;q&lt;/b&gt;" in html
    assert chat_format.to_text("see <https://x.test/run|logs>") == (
        "see logs (https://x.test/run)")
    assert chat_format.to_text("see <https://x.test/run|logs>", links=False) == (
        "see logs")
    assert chat_format.to_html("[x](javascript:alert(1))") == "x"


def test_email_html_mentions_link_to_the_profile():
    chat_format = _chat_format()
    style = chat_format.LINK_STYLE
    assert chat_format.to_html("hi @Bob") == (
        'hi <a href="https://forkmesh.com/@bob" style="%s">@Bob</a>' % style)
    assert chat_format.to_html("- @bob\n> @bob", "https://next.example/") == (
        '<ul style="%s"><li><a href="https://next.example/@bob" style="%s">@bob</a>'
        '</li></ul><blockquote style="%s"><a href="https://next.example/@bob" '
        'style="%s">@bob</a></blockquote>' % (
            chat_format.LIST_STYLE, style, chat_format.QUOTE_STYLE, style))
    assert chat_format.to_html("[see @bob](https://x.test/)") == (
        '<a href="https://x.test/" style="%s">see <strong>@bob</strong></a>' % style)
    assert chat_format.to_html("mail a@bob.com `@bob`").count("<a ") == 0


def test_notification_emails_format_their_body():
    assert "_chat_format = _LazyModule(\"chat_format\")" in ENTRY
    assert "_chat_format.to_text(body).split(\"\\n\")" in ENTRY
    assert "_chat_format.to_html(\n             body, _notification_item_url(base_url, \"/\"))" in ENTRY


def test_desktop_notifications_show_formatted_plain_text():
    assert "forkmesh::chatformat::toPlainText(message.text)" in MESSAGES
    assert "forkmesh::chatblocks::plainSummary(message.blocks)" in MESSAGES
    assert "forkmesh::chatformat::notificationText(" in MESSAGES
    assert "mentioned you \") + where" in MESSAGES
    assert "inline QString plainSummary(const QJsonArray &blocks)" in BLOCKS
    assert "chatformat::inlineHtml(match.captured(2), {}, dark)" in BLOCKS
    assert "forkmesh::chatformat::bodySource(plain, kMaxTextChars)" in OFFICE
    assert "'body-markup'" in ACTIONS
    assert "notificationServerReadsMarkup() ? body.toHtmlEscaped() : body" in ACTIONS
    assert "wrapComposerSelection(before, after);" in ISSUES
    assert "Qt::CTRL | Qt::SHIFT | Qt::Key_X" in ISSUES

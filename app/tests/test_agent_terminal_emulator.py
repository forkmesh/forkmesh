#!/usr/bin/env python3
"""Runtime behaviour of the streamed sign-in terminal's client half.

public/agent-terminal.js renders a node's pty in the browser so a provider
sign-in can be completed from the website instead of a `forkmesh://` handoff
that only resolves on a machine running the desktop app. The provider CLIs
draw a cursor-addressed, full-screen prompt, so "append the bytes to a log" is
not an option — the emulator's correctness is what decides whether the page
shows a sign-in or garbage. It is exercised here rather than asserted as text:

  * absolute cursor addressing, erase, and scroll-region redraws,
  * SGR colour/attribute runs surviving into the rendered HTML,
  * alternate-screen switches not polluting the scrollback,
  * the key encodings the CLIs' menus need, including application cursor mode,
  * markup in the stream being escaped rather than rendered,
  * the cursor being drawn where the operator is about to type,
  * a wrapped sign-in URL staying one clickable link to the whole URL.
"""

import json
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
TERMINAL = ROOT / "public" / "agent-terminal.js"

HARNESS = r'''
import { readFileSync } from "node:fs";
const source = readFileSync(process.argv[2], "utf8");
const url = "data:text/javascript;base64,"
  + Buffer.from(source).toString("base64");
const { createScreen, feed, renderScreen, keyToBytes } = await import(url);

const ESC = "\x1b";
const screen = createScreen(6, 40);
const scenario = process.argv[3];
const result = {};
// Every scenario but the cursor one asserts exact line text, so hide the
// cursor (DECTCEM) rather than teach each assertion to step around it.
if (scenario !== "cursor") feed(screen, `${ESC}[?25l`);

if (scenario === "redraw") {
  feed(screen, "first line\r\nsecond line");
  // Absolute addressing back over the top line, then erase to end of line:
  // exactly what an ink/React prompt does on every keystroke.
  feed(screen, `${ESC}[1;1H${ESC}[Krewritten`);
  result.lines = renderScreen(screen).split("\n");
  // A scroll region plus reverse index has to move only the region.
  feed(screen, `${ESC}[2;3r${ESC}[2;1H${ESC}[2Kabc${ESC}M`);
  result.afterScroll = renderScreen(screen).split("\n");
} else if (scenario === "styles") {
  feed(screen, `${ESC}[1;31mred${ESC}[0m plain`);
  result.html = renderScreen(screen).split("\n")[0];
  feed(screen, `${ESC}[2J${ESC}[H${ESC}[38;5;208m256${ESC}[0m`);
  result.indexed = renderScreen(screen).split("\n").at(-6);
} else if (scenario === "keyboard-modes") {
  feed(screen, `${ESC}[>4;2m${ESC}[>1u${ESC}[?u${ESC}[3;5Hplain`);
  result.plain = renderScreen(screen).split("\n")[2];
  feed(screen, `${ESC}[2J${ESC}[H${ESC}[4:3mcurly${ESC}[4:0m flat ${ESC}[38:2::1:2:3mrgb`);
  result.colon = renderScreen(screen).split("\n")[0];
} else if (scenario === "altscreen") {
  feed(screen, "history\r\n");
  feed(screen, `${ESC}[?1049h`);
  for (let i = 0; i < 12; i += 1) feed(screen, `menu ${i}\r\n`);
  result.altLines = renderScreen(screen).split("\n").length;
  feed(screen, `${ESC}[?1049l`);
  result.restored = renderScreen(screen).split("\n");
} else if (scenario === "escaping") {
  feed(screen, "<script>alert(1)</script> & done");
  // An OSC title sequence is consumed, not painted.
  feed(screen, `\r\n${ESC}]0;window title\x07visible`);
  result.html = renderScreen(screen).split("\n").slice(0, 2);
} else if (scenario === "cursor") {
  feed(screen, "code: ");
  result.atPrompt = renderScreen(screen).split("\n")[0];
  feed(screen, `${ESC}[?25l`);
  result.hidden = renderScreen(screen).split("\n")[0];
} else if (scenario === "hyperlink") {
  // Claude Code's sign-in line: an OSC 8 target, then the visible URL, far
  // wider than the grid. Copying the wrapped rows is how a browser used to
  // receive a URL cut off before its code_challenge.
  const url = "https://claude.com/cai/oauth/authorize?code=true&client_id=9d1c"
    + "&scope=user%3Ainference&code_challenge=r0RdaZpFHMZaIxRc"
    + "&code_challenge_method=S256&state=YTjm-5aspShAOZcz";
  result.url = url;
  const big = createScreen(12, 40);
  feed(big, `${ESC}[?25l`);
  feed(big, `visit: ${ESC}]8;;${url}\x07${ESC}[94m${url}${ESC}[39m`
    + `${ESC}]8;;\x07\r\nafter`);
  result.osc = renderScreen(big).split("\n");
  // Without OSC 8 (another CLI, or an older one) the soft-wrapped rows are
  // still found as one URL.
  const plain = createScreen(12, 40);
  feed(plain, `${ESC}[?25lgo ${url} now`);
  result.plain = renderScreen(plain).split("\n");
  const hostile = createScreen(6, 40);
  feed(hostile, `${ESC}[?25l${ESC}]8;;javascript:alert(1)\x07click${ESC}]8;;\x07`);
  result.hostile = renderScreen(hostile).split("\n")[0];
} else if (scenario === "keys") {
  const press = (key, extra = {}) =>
    keyToBytes({ key, ctrlKey: false, altKey: false, metaKey: false, ...extra },
      Boolean(extra.appCursorKeys));
  result.keys = {
    enter: press("Enter"),
    backspace: press("Backspace"),
    tab: press("Tab"),
    escape: press("Escape"),
    up: press("ArrowUp"),
    upApp: press("ArrowUp", { appCursorKeys: true }),
    pageUp: press("PageUp"),
    interrupt: press("c", { ctrlKey: true }),
    printable: press("a"),
    copyIsNotAKey: press("Shift"),
    metaIsTheBrowsers: press("v", { metaKey: true }),
  };
}

process.stdout.write(JSON.stringify(result));
'''


def _run(scenario):
    completed = subprocess.run(
        ["node", "--input-type=module", "-", str(TERMINAL), scenario],
        input=HARNESS, text=True, capture_output=True, check=True,
    )
    return json.loads(completed.stdout)


pytestmark = pytest.mark.skipif(
    shutil.which("node") is None, reason="node is required for this test")


def test_cursor_addressing_erase_and_scroll_regions_redraw_in_place():
    result = _run("redraw")
    assert result["lines"][0] == "rewritten"
    assert result["lines"][1] == "second line"
    # The reverse index scrolled inside rows 2-3 only, leaving row 1 alone.
    assert result["afterScroll"][0] == "rewritten"
    assert result["afterScroll"][1] == "&nbsp;"
    assert result["afterScroll"][2] == "abc"


def test_colour_and_attribute_runs_reach_the_rendered_line():
    result = _run("styles")
    assert "font-weight:600" in result["html"]
    assert "#e05561" in result["html"]  # SGR 31 from the base palette
    assert result["html"].endswith("plain")
    assert "rgb(255,135,0)" in result["indexed"]  # SGR 38;5;208


def test_keyboard_mode_sequences_leave_text_unstyled():
    result = _run("keyboard-modes")
    assert result["plain"] == "    plain"
    assert "underline" in result["colon"].split("flat")[0]
    assert "underline" not in result["colon"].split("flat")[1]
    assert "rgb(1,2,3)" in result["colon"]


def test_alternate_screen_redraws_do_not_pollute_the_scrollback():
    result = _run("altscreen")
    # An alt-screen menu that scrolls stays exactly the screen's height.
    assert result["altLines"] == 6
    assert result["restored"][0] == "history"


def test_stream_markup_is_escaped_and_osc_titles_are_dropped():
    result = _run("escaping")
    assert "&lt;script&gt;" in result["html"][0]
    assert "<script>" not in result["html"][0]
    assert "&amp; done" in result["html"][0]
    assert result["html"][1] == "visible"


def test_the_cursor_is_drawn_where_the_next_keystroke_lands():
    result = _run("cursor")
    # It sits one past the prompt, as a reversed cell, so a pasted code has a
    # visible destination.
    assert result["atPrompt"] == (
        "code: <span style=\"background:#c7c9cf;color:#0d0d10\"> </span>")
    # And DECTCEM still hides it when the remote program asks.
    assert result["hidden"] == "code:"


def test_a_wrapped_sign_in_url_links_to_the_whole_url():
    result = _run("hyperlink")
    href = 'href="' + result["url"].replace("&", "&amp;") + '"'
    for key in ("osc", "plain"):
        lines = [line for line in result[key] if line != "&nbsp;"]
        url_rows = [line for line in lines if "<a " in line]
        # The URL spans several 40-column rows; every piece of it opens the
        # complete URL, code_challenge included.
        assert len(url_rows) >= 4, lines
        assert all(href in line for line in url_rows), lines
    assert "after" in result["osc"]
    assert not any("<a " in line and "after" in line for line in result["osc"])
    assert any(line.endswith("</a> now") for line in result["plain"])
    # A link target that is not a web URL stays inert text.
    assert "<a " not in result["hostile"]
    assert result["hostile"] == "click"


def test_keys_are_encoded_the_way_the_provider_menus_expect():
    keys = _run("keys")["keys"]
    assert keys["enter"] == "\r"
    assert keys["backspace"] == "\x7f"
    assert keys["tab"] == "\t"
    assert keys["escape"] == "\x1b"
    assert keys["up"] == "\x1b[A"
    # DECCKM: the same key changes shape once the remote program asks for it.
    assert keys["upApp"] == "\x1bOA"
    assert keys["pageUp"] == "\x1b[5~"
    assert keys["interrupt"] == "\x03"
    assert keys["printable"] == "a"
    # Modifiers alone send nothing, and the browser keeps its own shortcuts.
    assert keys["copyIsNotAKey"] == ""
    assert keys["metaIsTheBrowsers"] == ""

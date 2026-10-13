#!/usr/bin/env python3
"""The /chat page must fit the *dynamic* mobile viewport.

On phones the browser's URL/tool bar shows and hides, changing the visible
viewport height. `100vh` reports the *largest* viewport, so a shell sized to
`100vh` overflows: its bottom (the message input and the tail of the scrollable
log) is pushed under the browser chrome, off-screen. The fix is to declare the
`100vh` fallback first and let `100dvh` (the dynamic viewport) win where it's
supported, so the input bar stays reachable and the full message log stays
scrollable on mobile web.
"""

from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[1] / "public"
CHAT_CSS = (PUBLIC / "chat.css").read_text(encoding="utf-8")


def _chat_shell_block():
    start = CHAT_CSS.index(".chat-shell {")
    return CHAT_CSS[start : CHAT_CSS.index("}", start)]


def test_chat_shell_declares_both_viewport_heights():
    block = _chat_shell_block()
    assert "height: 100dvh;" in block
    assert "height: 100vh;" in block


def test_dvh_overrides_vh_fallback_on_mobile():
    # CSS takes the last declaration a browser understands, so the dynamic
    # viewport unit (dvh) must come AFTER the vh fallback or 100vh wins and the
    # input bar / full history scroll off-screen on mobile.
    block = _chat_shell_block()
    assert block.index("height: 100vh;") < block.index("height: 100dvh;")

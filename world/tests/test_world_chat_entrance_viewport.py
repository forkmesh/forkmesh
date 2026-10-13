"""World chat must enter completely without moving the mobile viewport (#547)."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "world" / "public" / "world"
HTML = (WORLD / "index.html").read_text(encoding="utf-8")
CSS = (WORLD / "world.css").read_text(encoding="utf-8")
APP = (WORLD / "world.js").read_text(encoding="utf-8")


def _section(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin : source.index(end, begin)]


def test_mobile_keyboard_resizes_content_without_disabling_user_zoom():
    viewport = _section(HTML, '<meta name="viewport"', " />")
    assert "interactive-widget=resizes-content" in viewport
    assert "maximum-scale" not in viewport
    assert "user-scalable=no" not in viewport
    coarse = CSS[CSS.index("@media (pointer: coarse), (hover: none) {\n  .fm-world .world-touch-controls {"):]
    coarse = coarse[:coarse.index("\n}\n")]
    assert "#fullChatInput {\n    font-size: 16px;" in coarse


def test_expanding_the_island_never_focuses_the_composer():
    expand = APP[
        APP.index("  setHudIslandExpanded(expanded) {"):
        APP.index("\n  pinHudIslandFeed()")
    ]
    assert ".focus(" not in expand
    assert "log.scrollTop = log.scrollHeight;" in APP
    assert APP.count("focus({ preventScroll: true })") >= 3

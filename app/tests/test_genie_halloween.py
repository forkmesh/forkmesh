"""The Genie wears a witch hat and glows orange while the Halloween season
is on, and keeps its turban otherwise."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENIE = (ROOT.parent / "www" / "public" / "genie-spider.js").read_text(encoding="utf-8")


def test_the_genie_follows_the_season_every_frame():
    painter = GENIE[GENIE.index("  function painter(ctx, s) {"):]
    assert painter.index("const spooky = halloween();") < painter.index("const tint =")
    assert "function witchHat()" in GENIE
    assert "if (spooky) witchHat();\n    else turban();" in GENIE
    assert 'document.documentElement.dataset.season === "halloween"' in GENIE


def test_effects_and_chrome_use_the_season_glow():
    assert "html[data-season=halloween] .fm-genie-bubble" in GENIE
    assert "${glowRgb()}" in GENIE

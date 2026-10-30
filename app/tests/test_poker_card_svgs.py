from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "poker_card_svgs.py"
CARDS = ROOT / "marketplace" / "packages" / "poker" / "content" / "desktop" / "cards"


def test_card_svgs_match_generator():
    result = subprocess.run([sys.executable, str(TOOL), "--check"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_every_card_is_present():
    names = {path.name for path in CARDS.glob("*.svg")}
    ranks = ["2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K", "A"]
    expected = {f"{rank}_{suit}.svg" for rank in ranks for suit in "SHDC"} | {"card_back.svg"}
    assert names == expected


def test_suits_are_paths_not_font_glyphs():
    for path in CARDS.glob("*.svg"):
        text = path.read_text(encoding="utf-8")
        for glyph in "♠♣♥♦♤♧♡♢":
            assert glyph not in text, path.name
        assert "<path" in text, path.name

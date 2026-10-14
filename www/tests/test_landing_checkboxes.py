import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
START = ROOT / "app" / "public" / "start.html"


def test_start_page_checkboxes_use_ink_not_green():
    accents = re.findall(r"accent-color:\s*([^;]+);", START.read_text(encoding="utf-8"))
    assert accents == ["var(--ink-strong)"]


def test_landing_stylesheet_gives_checkboxes_the_ink_colour():
    css = (ROOT / "www" / "public" / "styles.css").read_text(encoding="utf-8")
    assert re.search(
        r':where\(input\[type="checkbox"\], input\[type="radio"\]\) \{\s*accent-color: var\(--foreground\);',
        css,
    )

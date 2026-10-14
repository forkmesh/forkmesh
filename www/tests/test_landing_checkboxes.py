import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
START = ROOT / "app" / "public" / "start.html"


def test_start_page_checkboxes_use_ink_not_green():
    accents = re.findall(r"accent-color:\s*([^;]+);", START.read_text(encoding="utf-8"))
    assert accents == ["var(--ink-strong)"]

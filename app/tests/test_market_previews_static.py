import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "app" / "tools"))

import build_site_assets


def test_market_images_are_static_previews_shipped_by_the_www_build(tmp_path):
    page = (ROOT / "www" / "public" / "market.html").read_text(encoding="utf-8")
    sources = re.findall(r'class="market-screenshot" src="([^"]+)"', page)
    assert sources
    assert not [s for s in sources if "/api/repo/" in s]
    build_site_assets.stage_market_previews(tmp_path)
    missing = [s for s in sources if not (tmp_path / s.lstrip("/")).is_file()]
    assert missing == []

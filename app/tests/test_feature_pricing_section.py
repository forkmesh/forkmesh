#!/usr/bin/env python3
"""Static homepage pricing and media contract tests."""

from html.parser import HTMLParser
from pathlib import Path
import re


PUBLIC = Path(__file__).resolve().parents[2] / "www" / "public"
INDEX_HTML = (PUBLIC / "index.html").read_text(encoding="utf-8")


class _VisibleMediaParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.paths = []

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        if tag not in {"img", "video"}:
            if tag == "link":
                rel = values.get("rel", "")
                href = values.get("href", "")
                if href and ("icon" in rel or rel == "manifest"):
                    self._append_path(href)
            return
        for name in ("src", "poster"):
            self._append_path(values.get(name))

    def _append_path(self, value):
        if value and value.startswith("/"):
            self.paths.append(value.split("?", 1)[0].split("#", 1)[0])


def test_community_rewards_band_is_gone_from_the_homepage():
    """The "Preserve code. Share community rewards." band was removed.

    Its wrapper went with it, so neither the section nor the rails survive.
    """
    for marker in (
        '<section id="pricing" class="forkmesh">',
        "FORKMESH - NETWORK PARTICIPATION",
        "Preserve code.",
        "Share <em>community rewards</em>.",
        'class="pattern-rails',
    ):
        assert marker not in INDEX_HTML


def test_visible_media_assets_exist():
    parser = _VisibleMediaParser()
    parser.feed(INDEX_HTML)
    parser.paths.extend(
        match.group(1) for match in re.finditer(r'url\(["\']?(/[^)"\']+)', INDEX_HTML)
    )

    missing = [
        path for path in parser.paths
        if not (PUBLIC / path.lstrip("/")).is_file()
    ]

    assert missing == []

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WWW = ROOT / "www" / "public"


def read(path):
    return path.read_text(encoding="utf-8")

DARK_PRIMARY_FILL = "linear-gradient(180deg, #ffffff 0%, #e8e8ea 100%)"
LIGHT_PRIMARY_FILL = "linear-gradient(180deg, #2c2c2c 0%, #222222 100%)"
LIGHT_PRIMARY_SHADOW = (
    "inset 0 1px 0 rgba(255, 255, 255, 0.17), 0 1px 2px rgba(0, 0, 0, 0.16)"
)
LIGHT_SECONDARY_FILL = "linear-gradient(180deg, #ffffff 0%, #fcfcfc 100%)"
LIGHT_SECONDARY_BORDER = "#e1e2e3"


def squash(text):
    return re.sub(r"\s+", "", text)


def test_share_card_buttons_use_the_textured_recipe():
    css = squash(read(WWW / "blog-social.css"))
    assert squash(f"--bs-primary-fill: {DARK_PRIMARY_FILL};") in css
    assert squash(f"--bs-primary-fill: {LIGHT_PRIMARY_FILL};") in css
    assert squash(f"--bs-primary-shadow: {LIGHT_PRIMARY_SHADOW};") in css
    assert squash(f"--bs-secondary-fill: {LIGHT_SECONDARY_FILL};") in css
    assert squash(f"--bs-secondary-border: {LIGHT_SECONDARY_BORDER};") in css


def test_article_bodies_style_code_quotes_tables_and_next_links():
    css = read(WWW / "feature-post.css")
    for rule in (
        ".body code {",
        ".body pre {",
        ".body blockquote {",
        ".body table {",
        ".body hr {",
        ".next a {",
    ):
        assert rule in css, rule

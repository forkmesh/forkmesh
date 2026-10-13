from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "app" / "public"


def read(path):
    return path.read_text(encoding="utf-8")


def assert_kit_hero(html, name):
    assert '<body class="kit-page">' in html, name
    assert 'class="kit-badge"' in html, name
    assert 'class="kit-title"' in html, name
    assert 'class="eyebrow"' not in html, name


def test_leaderboards_open_on_the_page_kit():
    html = read(APP / "leaderboards.html")
    assert_kit_hero(html, "leaderboards.html")
    assert "leaderboards-kicker" not in html
    assert ".leaderboards-hero .kit-hero {" in html


def test_start_and_review_buttons_use_the_shared_texture_tokens():
    start = read(APP / "start.html")
    launch = start.split(".launch-btn {", 1)[1].split("}", 1)[0]
    assert "background: var(--button-fill);" in launch
    assert "box-shadow: var(--button-shadow);" in launch
    reviews = read(APP / "market-reviews.html")
    assert (
        ".mr-button-primary { border-color: var(--button-border); "
        "background: var(--button-fill); box-shadow: var(--button-shadow);"
    ) in reviews

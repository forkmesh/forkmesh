from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WWW = ROOT / "www" / "public"


def read(path):
    return path.read_text(encoding="utf-8")


def assert_kit_hero(html, name):
    assert '<body class="kit-page">' in html, name
    assert 'class="kit-badge"' in html, name
    assert 'class="kit-title"' in html, name
    assert 'class="eyebrow"' not in html, name


def test_about_careers_and_changelog_open_on_the_page_kit():
    for name in ("about.html", "careers.html", "changelog.html"):
        assert_kit_hero(read(WWW / name), name)


def test_about_page_leaves_the_dark_only_mesh_theme():
    html = read(WWW / "about.html")
    assert 'href="/styles.css"' in html
    assert "tailwind.css" not in html
    assert "mesh-page-theme.css" not in html
    assert 'class="dark' not in html
    assert "rgba(150, 114, 238" not in html
    assert "rgba(63, 185, 80" not in html
    assert "mesh-grid" not in html
    assert "mesh-bg" not in html


def test_changelog_releases_use_a_timeline_without_green_dots():
    html = read(WWW / "changelog.html")
    assert html.count('<div class="release-meta">') == 6
    assert html.count('<div class="release-body">') == 6
    assert "var(--accent-bright)" not in html
    assert ".release::before" not in html

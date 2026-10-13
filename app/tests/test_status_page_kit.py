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


def test_status_page_keeps_its_probe_markers_on_the_kit():
    html = read(APP / "status.html")
    assert_kit_hero(html, "status.html")
    assert 'class="kit-title" id="status-title"' in html
    assert "/api/status/ws" in html
    assert 'class="status-stats"' in html
    assert "stats-grid" not in html
    assert "How this page measures uptime" in html

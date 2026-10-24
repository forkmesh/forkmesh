"""The Halloween theme on the dashboard: the shell turns orange through
theme.json, every page loads season.js, and the admin-only setting sits in
Settings, Appearance."""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
sys.path.insert(0, str(ROOT / "src"))

import dashboard_bundle  # noqa: E402
import dashboard_shell  # noqa: E402


def test_shell_writes_the_halloween_colours_from_theme_json():
    theme = json.loads((PUBLIC / "dashboard" / "theme.json").read_text(encoding="utf-8"))
    colors = theme["seasons"]["halloween"]
    assert set(colors) == {"cta", "success"}
    read = lambda rel: (PUBLIC / rel).read_text(encoding="utf-8")
    html = dashboard_shell.compose_page_from_reader(read, "home")
    assert "/*#season" not in html
    for mode in ("dark", "light"):
        for name, values in colors.items():
            red, green, blue = (int(part) for part in values[mode].split())
            assert red > green > blue, (name, mode)
            assert "--dashboard-%s-rgb: %s;" % (name, values[mode]) in html
    assert '<script src="/season.js' in html
    assert html.index("/season.js") < html.index("<body")


def test_dashboard_follows_the_season():
    assert "09-season.js" in dashboard_bundle.FRAGMENTS
    boot = (PUBLIC / "dashboard" / "js" / "10-boot.js").read_text(encoding="utf-8")
    assert "initSeasonSettings()" in boot and "watchSeason()" in boot
    settings = (PUBLIC / "dashboard" / "partials" / "views" / "settings.html").read_text(encoding="utf-8")
    assert re.search(r"<div data-season-setting hidden", settings)
    script = (ROOT.parent / "www" / "public" / "season.js").read_text(encoding="utf-8")
    assert '"/api/season"' in script and "prefers-reduced-motion" in script

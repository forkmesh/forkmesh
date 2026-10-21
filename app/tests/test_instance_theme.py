import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tools"))

import apply_instance_brand  # noqa: E402
import dashboard_shell  # noqa: E402
import instance_theme  # noqa: E402

APP_RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
PUBLIC = ROOT / "public"
RAW_SHELL = (PUBLIC / "dashboard" / "shell.html").read_text(encoding="utf-8")
THEME = json.loads((PUBLIC / dashboard_shell.THEME_PATH).read_text(encoding="utf-8"))
SHELL = dashboard_shell.apply_theme(
    RAW_SHELL, lambda path: (PUBLIC / path).read_text(encoding="utf-8"))


def test_every_theme_token_is_a_token_the_dashboard_defines():
    assert not dashboard_shell.THEME_TOKEN_RE.search(SHELL)
    for name in instance_theme.RGB_TOKENS:
        assert "--dashboard-%s-rgb:" % name in SHELL
        assert set(THEME["colors"].get(name, {})) >= {"dark", "light"}, name
    for name in instance_theme.HEX_TOKENS:
        assert "--dashboard-%s:" % name in SHELL
    for alias, _ in instance_theme.ALIASES:
        assert "\n        --%s:" % alias in SHELL
    for selector in ('[data-dashboard-theme="dark"]', '[data-dashboard-theme="light"]'):
        assert selector in SHELL


def test_crimson_preset_is_valid_and_renders_both_modes():
    theme = instance_theme.clean("crimson")
    assert theme == instance_theme.clean(instance_theme.PRESETS["crimson"])
    assert theme["dark"]["primary"] == "#ef4444"
    assert theme["light"]["primary"] == "#dc2626"
    css = instance_theme.css(theme)
    dark, light = css.strip().split("\n")
    assert dark.startswith(':root,[data-dashboard-theme="dark"]{')
    assert light.startswith('[data-dashboard-theme="light"]{')
    assert "--dashboard-primary-rgb:239 68 68;" in dark
    assert "--dashboard-link:#f87171;" in dark
    assert "--primary:#ef4444;" in dark
    assert "--dashboard-primary-rgb:220 38 38;" in light
    assert instance_theme.parse(instance_theme.dump(theme)) == theme


def test_a_partial_theme_only_overrides_what_it_names():
    theme = instance_theme.clean({"dark": {"primary": "#AABBCC"}})
    assert theme == {"dark": {"primary": "#aabbcc"}}
    assert instance_theme.css(theme) == (
        ':root,[data-dashboard-theme="dark"]'
        "{--dashboard-primary-rgb:170 187 204;--primary:#aabbcc}\n")
    assert instance_theme.css({}) == ""


@pytest.mark.parametrize("value", [
    "nope",
    ["dark"],
    {"sepia": {}},
    {"dark": "red"},
    {"dark": {"layout": "#000000"}},
    {"dark": {"primary": "red"}},
    {"dark": {"primary": "#fff"}},
    {"dark": {"primary": "#000000;} body{display:none"}},
    {"dark": {"primary": "url(https://example.com/x)"}},
])
def test_theme_refuses_anything_but_known_tokens_and_hex_colours(value):
    with pytest.raises(instance_theme.ThemeError):
        instance_theme.clean(value)


def test_a_damaged_stored_theme_reads_as_no_theme():
    assert instance_theme.parse("") == {}
    assert instance_theme.parse("{") == {}
    assert instance_theme.parse(json.dumps({"dark": {"primary": "red"}})) == {}


def test_brand_tool_links_the_theme_stylesheet_once_after_the_page_styles(tmp_path):
    page = tmp_path / "index.html"
    page.write_text(
        "<!doctype html><html><head><title>ForkMesh</title>"
        "<style>:root{--dashboard-primary-rgb: 35 134 54;}</style>"
        "</head><body></body></html>")
    apply_instance_brand.apply(tmp_path, "Acme Code")
    apply_instance_brand.apply(tmp_path, "Acme Code")
    text = page.read_text()
    assert text.count("/instance-theme.css") == 1
    assert text.index("</style>") < text.index("/instance-theme.css") < text.index("</head>")


def test_routes_serve_the_theme_only_on_a_branded_instance():
    route = APP_RUNTIME.split("    async def _route(self, request, url):", 1)[1]
    head = route.split("delegated = self._world_ingress_redirect", 1)[0]
    assert '"theme": await self._instance_theme()' in head
    assert 'url.path == "/instance-theme.css" and instance_branded(self.env)' in head
    assert 'url.path == "/api/instance/theme" and instance_branded(self.env)' in head
    reader = APP_RUNTIME.split("    async def _instance_theme(self):", 1)[1]
    assert reader.lstrip().startswith("if not instance_branded(self.env):")
    writer = APP_RUNTIME.split("    async def _instance_theme_write(self, request):", 1)[1]
    writer = writer.split("    async def _route(", 1)[0]
    assert writer.index("_is_admin(self.env, actor)") < writer.index("INSERT INTO instance_settings")
    assert "_account_session_record(" in writer
    assert writer.index("verify_password(") < writer.index("INSERT INTO instance_settings")
    assert writer.index("_login_locked_until(") < writer.index("verify_password(")


def test_named_themes_load_from_the_catalogue():
    ids = [entry["id"] for entry in instance_theme.catalogue()]
    assert ids[:2] == ["default", "blt"]
    assert {"cloudflare", "google"} <= set(ids)
    assert all(instance_theme.theme_ready(name) for name in ("blt", "cloudflare", "google"))
    raw = json.loads((PUBLIC / "instance-themes" / "themes.json").read_text(encoding="utf-8"))
    for entry in raw["themes"]:
        assert 0 < len(entry["palette"]) <= 8, entry["id"]
    for entry in instance_theme.catalogue():
        assert entry["palette"], entry["id"]
    assert instance_theme.theme_ready("blt")
    for theme in instance_theme.THEMES.values():
        if theme["status"] != "ready" or theme["id"] == instance_theme.DEFAULT_THEME:
            continue
        assert theme["stylesheet"], theme["id"]
        for mode in instance_theme.MODES:
            assert set(theme["colors"][mode]) == set(instance_theme.THEME_TOKENS), theme["id"]
    for name in instance_theme.THEME_RGB_TOKENS:
        assert set(THEME["colors"].get(name, {})) >= {"dark", "light"}, name


def test_a_ready_theme_is_stored_by_name_and_rendered_with_its_stylesheet():
    assert instance_theme.clean("blt") == {"preset": "blt"}
    assert instance_theme.clean("default") == {}
    assert instance_theme.clean({"preset": "default"}) == {}
    theme = instance_theme.clean({"preset": "BLT", "dark": {"primary": "#AA0000"}})
    assert theme == {"preset": "blt", "dark": {"primary": "#aa0000"}}
    assert instance_theme.parse(instance_theme.dump(theme)) == theme
    css = instance_theme.css(theme)
    assert "--dashboard-cta-rgb:220 38 38" in css
    assert "border-radius" in css
    assert css.rindex("--dashboard-primary-rgb:170 0 0") > css.index("--dashboard-primary-rgb:220 38 38")
    assert instance_theme.preview_css("blt") == instance_theme.css({"preset": "blt"})
    assert instance_theme.preview_css("default") == ""
    assert instance_theme.preview_css("nope") is None


@pytest.mark.parametrize("value", ["soon", "nope", {"preset": "nope"}, {"preset": "soon"}])
def test_planned_or_unknown_themes_are_refused(value, monkeypatch):
    monkeypatch.setitem(instance_theme.THEMES, "soon", {
        "id": "soon", "name": "Soon", "description": "", "status": "planned",
        "palette": [], "colors": {}, "stylesheet": ""})
    with pytest.raises(instance_theme.ThemeError):
        instance_theme.clean(value)
    assert instance_theme.preview_css("soon") is None


def test_a_missing_catalogue_leaves_only_colour_themes(tmp_path):
    assert instance_theme._load_themes(tmp_path) == {}
    (tmp_path / "themes.json").write_text(json.dumps({"themes": [
        {"id": "x", "status": "ready", "stylesheet": "../secret.css",
         "colors": {"dark": {"primary": "red", "border": "#ABCDEF"}}},
        {"id": "Bad Id"}]}))
    themes = instance_theme._load_themes(tmp_path)
    assert list(themes) == ["x"]
    assert themes["x"]["stylesheet"] == ""
    assert themes["x"]["colors"] == {"dark": {"border": "#abcdef"}}

def test_settings_offer_the_instance_theme_to_admins_only():
    settings = (PUBLIC / "dashboard" / "partials" / "views" / "settings.html").read_text(encoding="utf-8")
    script = (PUBLIC / "dashboard" / "js" / "04-account.js").read_text(encoding="utf-8")
    assert '<div data-instance-theme-nav class="hidden grid">' in settings
    assert 'data-settings-section="instance-theme"' in settings
    assert 'data-instance-theme-password type="password"' in settings
    assert 'nav?.classList.toggle("hidden", !(session?.isAdmin === true && dashboardOnInstance()))' in script
    assert 'body: JSON.stringify({ theme: id, password })' in script

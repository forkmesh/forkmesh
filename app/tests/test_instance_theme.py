import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tools"))

import apply_instance_brand  # noqa: E402
import instance_theme  # noqa: E402

APP_RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
SHELL = (ROOT / "public" / "dashboard" / "shell.html").read_text(encoding="utf-8")


def test_every_theme_token_is_a_token_the_dashboard_defines():
    for name in instance_theme.RGB_TOKENS:
        assert "--dashboard-%s-rgb:" % name in SHELL
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
    assert "_request_origin_allowed(self.env, request)" in writer

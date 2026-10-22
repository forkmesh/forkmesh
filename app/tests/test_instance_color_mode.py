import asyncio
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "host"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import admin_shell  # noqa: E402
import apply_instance_brand  # noqa: E402
import instance_console as console_module  # noqa: E402
import instance_theme  # noqa: E402
from forkmesh_host import read_surfaces  # noqa: E402
from test_instance_registry import FakeRequest, bind_console  # noqa: E402

PUBLIC = ROOT / "public"
SHELL = (PUBLIC / "dashboard" / "shell.html").read_text(encoding="utf-8")
HELPERS = (PUBLIC / "dashboard" / "js" / "02-helpers.js").read_text(encoding="utf-8")
BOOT = (PUBLIC / "dashboard" / "js" / "10-boot.js").read_text(encoding="utf-8")
SETTINGS = (PUBLIC / "dashboard" / "partials" / "views" / "settings.html").read_text(
    encoding="utf-8")
GITHUB_FONT = ('-apple-system, BlinkMacSystemFont, "Segoe UI", "Noto Sans", Helvetica, '
               'Arial, sans-serif, "Apple Color Emoji", "Segoe UI Emoji"')
UNICODE_SPACES = re.compile(r"[\u00a0\u1680\u2000-\u200f\u2028-\u202f\u205f\u3000\ufeff]")


def test_github_preset_follows_the_system_with_githubs_palette():
    assert instance_theme.clean("github") == {"preset": "github"}
    assert instance_theme.clean("GitHub") == {"preset": "github"}
    assert instance_theme.color_mode({"preset": "github"}) == "system"
    entry = instance_theme.THEMES["github"]
    assert entry["status"] == "ready" and entry["mode"] == "system"
    assert "github" in [item["id"] for item in instance_theme.catalogue()]
    colours = entry["colors"]
    assert {colours["light"][name] for name in ("background", "foreground", "border",
                                                 "link", "cta", "destructive")} == {
        "#ffffff", "#1f2328", "#d0d7de", "#0969da", "#1f883d", "#d1242f"}
    assert {colours["dark"][name] for name in ("background", "foreground", "border",
                                                "link", "cta", "destructive")} == {
        "#0d1117", "#f0f6fc", "#3d444d", "#4493f8", "#238636", "#f85149"}
    css = instance_theme.css({"preset": "github"})
    assert ':root,[data-dashboard-theme="dark"]{--dashboard-header-rgb:1 4 9;' in css
    assert "--dashboard-background-rgb:13 17 23" in css
    assert '[data-dashboard-theme="light"]{--dashboard-header-rgb:246 248 250;' in css
    assert ":root{--instance-color-mode:system}" in css
    assert GITHUB_FONT in css
    media = css.split("@media (prefers-color-scheme: light){", 1)[1]
    assert media.startswith("html:not([data-dashboard-theme-resolved]){")
    for line in ("--dashboard-background-rgb:255 255 255", "--dashboard-cta-rgb:31 136 61",
                 "--dashboard-link:#0969da", "--background:#ffffff", "color-scheme:light"):
        assert line in media
    assert "url(" not in css and "@import" not in css and "http" not in css
    assert not UNICODE_SPACES.search(css)


def test_the_colour_mode_is_stored_only_when_it_differs_from_the_theme():
    assert instance_theme.clean({"preset": "github", "mode": "system"}) == {"preset": "github"}
    assert instance_theme.clean({"preset": "github", "mode": "Dark"}) == {
        "preset": "github", "mode": "dark"}
    assert instance_theme.clean({"mode": "dark"}) == {}
    assert instance_theme.clean({"mode": ""}) == {}
    custom = instance_theme.clean({"mode": "system", "dark": {"primary": "#1a9be6"}})
    assert custom == {"mode": "system", "dark": {"primary": "#1a9be6"}}
    assert instance_theme.parse(instance_theme.dump(custom)) == custom
    with pytest.raises(instance_theme.ThemeError):
        instance_theme.clean({"mode": "sepia"})
    assert instance_theme.css({}) == ""
    assert "--instance-color-mode" not in instance_theme.css({"preset": "github", "mode": "dark"})
    assert "--instance-color-mode" not in instance_theme.css({"preset": "blt"})
    light = instance_theme.css({"mode": "light"})
    assert light.startswith(":root{--instance-color-mode:light}\nhtml:not(")
    assert "@media" not in light
    system = instance_theme.css(custom)
    assert "--dashboard-primary-rgb:26 155 230" in system
    assert "@media (prefers-color-scheme: light){html:not([data-dashboard-theme-resolved])" in system


def test_the_no_script_light_colours_start_from_the_shells_own():
    light_block = SHELL.split('[data-dashboard-theme="light"] {', 1)[1].split("}", 1)[0]
    for name, value in instance_theme.BASE_LIGHT_HEX.items():
        assert "--dashboard-%s: %s;" % (name, value) in light_block
    assert instance_theme.BASE_LIGHT["background"] == "#ffffff"
    assert set(instance_theme.BASE_LIGHT) >= set(instance_theme.THEME_TOKENS)


def test_brand_tool_adds_the_colour_mode_script_once_after_the_theme(tmp_path):
    page = tmp_path / "index.html"
    page.write_text("<!doctype html><html><head><style>a{}</style></head><body></body></html>")
    apply_instance_brand.apply(tmp_path, "example")
    apply_instance_brand.apply(tmp_path, "example")
    text = page.read_text()
    assert text.count("data-instance-color-mode") == 1
    theme = text.index("/instance-theme.css")
    mode = text.index("<script data-instance-color-mode>")
    assert theme < mode < text.index("/instance-brand.js") < text.index("</head>")
    assert not UNICODE_SPACES.search(apply_instance_brand.MODE_TAG)


def _node(source, mode="", light=False, stored=None, cookie="", search="",
          after="", initial=None):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    script = """
const attrs = new Map(Object.entries(%(initial)s));
const name = (key) => "data-" + String(key).replace(/[A-Z]/g, (c) => "-" + c.toLowerCase());
const root = {
  style: {},
  setAttribute: (key, value) => attrs.set(key, String(value)),
  getAttribute: (key) => attrs.has(key) ? attrs.get(key) : null,
  dataset: new Proxy({}, {
    set: (_t, key, value) => { attrs.set(name(key), String(value)); return true; },
    get: (_t, key) => attrs.has(name(key)) ? attrs.get(name(key)) : undefined,
  }),
};
const meta = { content: "", setAttribute(_key, value) { this.content = value; } };
globalThis.document = { documentElement: root, cookie: %(cookie)s, querySelector: () => meta };
globalThis.location = { search: %(search)s, hostname: "example.example.org", protocol: "https:" };
const storage = new Map(Object.entries(%(stored)s));
globalThis.localStorage = {
  getItem: (key) => storage.has(key) ? storage.get(key) : null,
  setItem: (key, value) => storage.set(key, String(value)),
};
globalThis.getComputedStyle = () => ({ getPropertyValue: (key) => key === "--instance-color-mode" ? %(mode)s : "" });
const listeners = [];
const query = { matches: %(light)s, addEventListener: (_event, listener) => listeners.push(listener) };
globalThis.matchMedia = () => query;
%(source)s
%(after)s
console.log(JSON.stringify({ attrs: Object.fromEntries(attrs), scheme: root.style.colorScheme || "",
  meta: meta.content, listeners: listeners.length, storage: Object.fromEntries(storage) }));
""" % {"initial": json.dumps(initial or {"data-dashboard-theme": "dark"}),
       "cookie": json.dumps(cookie), "search": json.dumps(search),
       "stored": json.dumps(stored or {}), "mode": json.dumps(mode),
       "light": "true" if light else "false", "source": source, "after": after}
    out = subprocess.run([node, "-e", script], check=True, capture_output=True,
                         text=True, timeout=30)
    return json.loads(out.stdout.strip().splitlines()[-1])


MODE_SCRIPT = apply_instance_brand.MODE_TAG.split(">", 1)[1].rsplit("</script>", 1)[0]


@pytest.mark.parametrize("mode,light,stored,cookie,theme", [
    ("system", True, None, "", "light"),
    ("system", False, None, "", "dark"),
    ("system", True, {"forkmesh.dashboard.theme": "dark"}, "", "dark"),
    ("system", False, None, "forkmesh_theme=light", "light"),
    ("", True, {"forkmesh.theme": "system"}, "", "light"),
    ("light", False, None, "", "light"),
    ("", True, None, "", "dark"),
])
def test_the_colour_mode_script_picks_the_theme(mode, light, stored, cookie, theme):
    result = _node(MODE_SCRIPT, mode=mode, light=light, stored=stored, cookie=cookie)
    assert result["attrs"]["data-dashboard-theme"] == theme
    assert result["attrs"]["data-dashboard-theme-resolved"] == ""
    assert result["listeners"] == 1


def test_the_colour_mode_script_follows_a_system_change():
    after = "query.matches = true; listeners.forEach((listener) => listener());"
    result = _node(MODE_SCRIPT, mode="system", light=False, after=after)
    assert result["attrs"]["data-dashboard-theme"] == "light"
    assert result["scheme"] == "light" and result["meta"] == "#f6f8fb"


def _shell_theme_script():
    start = SHELL.index("      (() => {\n        const key = \"forkmesh.dashboard.theme\";")
    return SHELL[start:SHELL.index("      })();", start) + len("      })();")]


@pytest.mark.parametrize("stored,light,search,theme", [
    (None, True, "", "dark"),
    ({"forkmesh.dashboard.theme": "system"}, True, "", "light"),
    ({"forkmesh.dashboard.theme": "system"}, False, "", "dark"),
    ({"forkmesh.dashboard.theme": "light"}, False, "", "light"),
    ({"forkmesh.dashboard.theme": "system"}, True, "?theme=dark", "dark"),
])
def test_the_shell_reads_a_system_choice_before_the_first_paint(stored, light, search, theme):
    result = _node(_shell_theme_script(), light=light, stored=stored, search=search,
                   initial={})
    assert result["attrs"]["data-dashboard-theme"] == theme
    assert result["attrs"]["data-dashboard-theme-resolved"] == ""


def _helpers():
    start = HELPERS.index("  function normalizeDashboardTheme(")
    end = HELPERS.index("  function renderGenieSetting(")
    return ('const DASHBOARD_THEME_KEY = "forkmesh.dashboard.theme";\n'
            "const buttons = [];\nconst $$ = () => buttons;\n"
            "const status = { textContent: '' };\nconst $ = () => status;\n"
            + HELPERS[start:end])


@pytest.mark.parametrize("mode,light,stored,theme", [
    ("system", True, None, "light"),
    ("system", True, {"forkmesh.dashboard.theme": "dark"}, "dark"),
    ("", True, None, "dark"),
    ("", True, {"forkmesh.dashboard.theme": "system"}, "light"),
])
def test_the_dashboard_keeps_the_theme_the_page_picked(mode, light, stored, theme):
    result = _node(_helpers(), mode=mode, light=light, stored=stored,
                   after="applyDashboardTheme(readDashboardTheme()); followSystemDashboardTheme();")
    assert result["attrs"]["data-dashboard-theme"] == theme
    assert result["listeners"] == 1


def test_choosing_system_in_settings_stores_it_and_follows_the_system():
    after = ('saveDashboardTheme("system"); followSystemDashboardTheme();'
             "query.matches = false; listeners.forEach((listener) => listener());")
    result = _node(_helpers(), light=True, after=after)
    assert result["storage"]["forkmesh.dashboard.theme"] == "system"
    assert result["storage"]["forkmesh.theme"] == "system"
    assert result["attrs"]["data-dashboard-theme"] == "dark"
    assert 'data-appearance-theme="system"' in SETTINGS
    assert "applyDashboardTheme(readDashboardTheme());\n    followSystemDashboardTheme();" in BOOT


def _theme_console(monkeypatch, form=None, rows=None, branded=True):
    rows, _directory = bind_console(monkeypatch, branded=branded, rows=rows)
    env = SimpleNamespace(ADMIN_PATH="secret", INSTANCE_NAME="example",
                          PUBLIC_BASE_URL="https://example.example.org")
    if form is None:
        request = FakeRequest("https://example.example.org/secret?view=site-theme")
    else:
        request = FakeRequest("https://example.example.org/secret", "POST",
                              dict(form, action="save_theme"))
    answer = asyncio.run(console_module.admin_handler(
        env, request, {"view": ["site-theme"]}, "<csrf>", "", "ada"))
    return answer, rows


def test_the_console_switches_the_site_theme(monkeypatch):
    custom = json.dumps({"dark": {"primary": "#1a9be6"}, "light": {"primary": "#0b5cad"}})
    answer, rows = _theme_console(monkeypatch, rows={"theme": custom})
    assert 'value="custom" checked' in answer.body and 'value="github"' in answer.body
    assert "save_theme" in answer.body and 'name="mode"' in answer.body
    answer, rows = _theme_console(monkeypatch, {"csrf": "ok", "preset": "custom",
                                                "mode": "system"}, rows=rows)
    assert answer.headers["location"] == "/secret?view=site-theme&notice=theme_saved"
    assert json.loads(rows["theme"])["mode"] == "system"
    assert json.loads(rows["theme"])["dark"] == {"primary": "#1a9be6"}
    answer, rows = _theme_console(monkeypatch, {"csrf": "ok", "preset": "github", "mode": ""},
                                  rows=rows)
    assert json.loads(rows["theme"]) == {"preset": "github"}
    answer, rows = _theme_console(monkeypatch, rows=rows)
    assert 'value="github" checked' in answer.body and 'value="custom"' not in answer.body
    assert "following each visitor&#x27;s system setting" in answer.body
    for form in ({"preset": "nope", "mode": ""}, {"preset": "github", "mode": "sepia"}):
        answer, rows = _theme_console(monkeypatch, dict(form, csrf="ok"), rows=rows)
        assert answer.headers["location"].endswith("notice=theme_invalid")
    assert json.loads(rows["theme"]) == {"preset": "github"}
    answer, rows = _theme_console(monkeypatch, {"csrf": "bad", "preset": "default", "mode": ""},
                                  rows=rows)
    assert answer.headers["location"].endswith("notice=csrf")
    answer, rows = _theme_console(monkeypatch, {"csrf": "ok", "preset": "github", "mode": ""},
                                  branded=False)
    assert answer.headers["location"].endswith("notice=theme_core") and rows == {}
    assert ("site-theme", "Site theme", "system", ("console", "site-theme")) in dict(
        admin_shell.NAV)["Platform"]


def test_lite_and_forum_pages_wear_githubs_palette_and_fonts():
    style = read_surfaces.STYLE
    assert "@media(prefers-color-scheme:dark)" in style
    for colour in ("--b:#fff", "--l:#0969da", "--r:#d0d7de", "--d:#d1242f", "--p:#1f883d",
                   "--b:#0d1117", "--l:#4493f8", "--p:#238636"):
        assert colour in style
    assert GITHUB_FONT.replace(", ", ",") in style
    assert "background:var(--p)" in read_surfaces.HOME_STYLE

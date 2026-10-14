import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
MARKETPLACE = ROOT.parent / "marketplace" / "packages"
THEME = json.loads((PUBLIC / "dashboard" / "theme.json").read_text(encoding="utf-8"))
SHELL = (PUBLIC / "dashboard" / "shell.html").read_text(encoding="utf-8")

if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import dashboard_shell  # noqa: E402


def _rgb(value):
    return tuple(int(part) for part in value.split())


def _sources():
    roots = [
        PUBLIC / "dashboard" / "partials",
        PUBLIC / "dashboard" / "js",
    ]
    files = [
        PUBLIC / "dashboard" / "shell.html",
        PUBLIC / "dashboard-composer.css",
        PUBLIC / "dashboard-composer.js",
    ]
    for root in roots:
        files.extend(sorted(p for p in root.rglob("*") if p.suffix in {".html", ".js"}))
    files.extend(sorted(MARKETPLACE.glob("*/content/web/**/*.html")))
    files.extend(sorted(MARKETPLACE.glob("*/content/web/**/*.js")))
    return files


def test_every_colour_has_a_dark_and_a_light_value():
    for name, values in THEME["colors"].items():
        assert set(values) == {"dark", "light"}, name
        for mode in ("dark", "light"):
            assert len(_rgb(values[mode])) == 3, (name, mode)


def test_primary_is_blue_and_green_is_reserved_for_cta_and_success():
    for mode in ("dark", "light"):
        red, green, blue = _rgb(THEME["colors"]["primary"][mode])
        assert blue > red and blue > green
        for name in ("cta", "success"):
            red, green, blue = _rgb(THEME["colors"][name][mode])
            assert green > red and green > blue
    for role in ("primary", "accent", "cta", "success", "muted-foreground"):
        assert THEME["roles"][role]


def test_shell_takes_its_colours_from_theme_json_only():
    assert "/*#theme dark*/" in SHELL
    assert "/*#theme light*/" in SHELL
    assert not re.search(r"--dashboard-[a-z-]+-rgb:\s*\d", SHELL)


def test_composed_pages_carry_the_theme_tokens():
    read = lambda rel: (PUBLIC / rel).read_text(encoding="utf-8")
    html = dashboard_shell.compose_page_from_reader(read, "home")
    assert "/*#theme" not in html
    for mode in ("dark", "light"):
        assert "--dashboard-primary-rgb: %s;" % THEME["colors"]["primary"][mode] in html
        assert "--dashboard-cta-rgb: %s;" % THEME["colors"]["cta"][mode] in html
    for name, value in THEME["density"].items():
        assert "--dashboard-%s: %s;" % (name, value) in html


def test_tailwind_colours_come_from_theme_json():
    config = (ROOT / "tailwind.dashboard.config.js").read_text(encoding="utf-8")
    assert 'require("./public/dashboard/theme.json")' in config
    result = subprocess.run(
        ["node", "-e", "console.log(JSON.stringify(Object.keys(require('./tailwind.dashboard.config.js').theme.extend.colors)))"],
        cwd=ROOT, capture_output=True, text=True, check=True,
    )
    names = set(json.loads(result.stdout))
    assert set(THEME["colors"]) <= names


def test_no_green_on_focus_selection_or_links():
    pattern = re.compile(
        r"((focus|focus-visible|focus-within|checked|aria-selected|aria-pressed|aria-checked|aria-current|data-\[state=active\]):[a-z0-9/:-]*(emerald|green)-[0-9])"
        r"|((ring|outline|accent|decoration)-(emerald|green)-[0-9])"
        r"|((text|border|ring|outline|accent|fill|stroke|decoration)-cta(?![-\w]))"
        r"|(accent-color:[^;]*(cta|#238636|#2ea043|#1f883d|emerald|green))"
        r"|((aria-checked|aria-pressed|aria-selected)=\"true\"\][^{]*\{[^}]*(green|success|cta|#238636|#2ea043|#3fb950|#1f883d))"
    )
    offenders = []
    for path in _sources():
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if pattern.search(line):
                offenders.append("%s:%d" % (path.relative_to(ROOT.parent), number))
    assert offenders == []


def test_agents_are_pointed_at_the_theme_file():
    rules = (ROOT.parent / "AGENTS.md").read_text(encoding="utf-8")
    assert "app/public/dashboard/theme.json" in rules

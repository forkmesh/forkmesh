import re
import sys
from pathlib import Path

import pytest


WWW = Path(__file__).resolve().parents[1]
REPOSITORY = WWW.parent
PUBLIC = WWW / "public"
APP_PUBLIC = REPOSITORY / "app" / "public"
sys.path.insert(0, str(REPOSITORY / "app" / "tools"))
sys.path.insert(0, str(REPOSITORY / "app" / "src"))

import dashboard_shell  # noqa: E402
import site_templates  # noqa: E402


SPIDER = '<script src="/genie-spider.js?v=${version}" defer></script>'
GENERATED_PAGES = {"docs/git-compatibility/index.html"}
APP_PAGES_WITHOUT_SPIDER = {"chat.html", "oauth-authorize.html", "pitch-deck/deck.html"}


def environment(tmp_path, files):
    for name, text in files.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    return site_templates.Environment(tmp_path)


def test_public_pages_are_rendered_from_the_templates():
    assert site_templates.stale_pages() == []


def test_every_hand_written_page_has_a_template():
    pages = {path.relative_to(PUBLIC).as_posix() for path in PUBLIC.rglob("*.html")}
    templates = {path.relative_to(site_templates.PAGES).as_posix() for path in site_templates.page_templates()}
    assert pages - templates == GENERATED_PAGES
    assert templates <= pages


def test_every_page_extends_the_base():
    environment = site_templates.Environment()
    for path in site_templates.page_templates():
        name = path.relative_to(site_templates.TEMPLATES).as_posix()
        chain = [environment.load(name)]
        while chain[-1].parent:
            chain.append(environment.load(chain[-1].parent))
        assert chain[-1].name == "base.html", name


@pytest.mark.parametrize("path", sorted(PUBLIC.rglob("*.html")), ids=lambda p: p.relative_to(PUBLIC).as_posix())
def test_every_site_page_loads_the_spider(path):
    text = path.read_text(encoding="utf-8")
    assert text.count(SPIDER) == 1
    assert text.index(SPIDER) < text.index("</head>")


def test_share_images_exist():
    missing = []
    for path in sorted(PUBLIC.rglob("*.html")):
        text = path.read_text(encoding="utf-8")
        for image in re.findall(r'(?:og|twitter):image"\s+content="https://forkmesh\.com(/[^"?#]+)', text):
            if not (PUBLIC / image.lstrip("/")).is_file():
                missing.append((path.relative_to(PUBLIC).as_posix(), image))
    assert missing == []


def test_app_pages_load_the_spider():
    for path in sorted(APP_PUBLIC.rglob("*.html")):
        relative = path.relative_to(APP_PUBLIC).as_posix()
        if "/partials/" in relative or relative == "dashboard/shell.html":
            continue
        text = path.read_text(encoding="utf-8")
        if relative in APP_PAGES_WITHOUT_SPIDER:
            assert SPIDER not in text, relative
        elif "</head>" in text:
            assert SPIDER in text, relative


def test_dashboard_pages_load_the_spider():
    read = lambda rel: (APP_PUBLIC / rel).read_text(encoding="utf-8")
    assert SPIDER in dashboard_shell.compose_page_from_reader(read, "agents")


def test_spider_is_shared_and_cache_busted():
    import build_site_assets

    assert "genie-spider.js" in build_site_assets.SHARED_FILES
    assert "genie-spider.js" in dashboard_shell.CACHE_BUSTED_ASSETS


def test_blocks_override_and_super(tmp_path):
    env = environment(tmp_path, {
        "base.html": "<t>{% block title %}Base{% endblock %}</t>\n{% block body %}\nbase body\n{% endblock %}\n",
        "layout.html": '{% extends "base.html" %}\n{% block body %}\n<main>\n{{ super() }}{% block content %}\n{% endblock %}\n</main>\n{% endblock %}\n',
        "page.html": '{% extends "layout.html" %}\n{% block title %}Page{% endblock %}\n{% block content %}\n  hi\n{% endblock %}\n',
    })
    assert env.render("page.html") == "<t>Page</t>\n<main>\nbase body\n  hi\n</main>\n"


def test_variables_includes_and_conditions(tmp_path):
    env = environment(tmp_path, {
        "base.html": '<h1>{{ heading }}</h1>\n{% include "part.html" %}\n{% if note %}\nnote: {{ note }}\n{% else %}\nno note\n{% endif %}\n',
        "part.html": "<p>{{ heading }} part</p>\n",
        "page.html": '{% extends "base.html" %}\n{% set heading = "Hello &amp; welcome" %}\n',
    })
    assert env.render("page.html") == "<h1>Hello &amp; welcome</h1>\n<p>Hello &amp; welcome part</p>\nno note\n"
    assert env.render("page.html", {"note": "x"}).endswith("note: x\n")


@pytest.mark.parametrize("files", [
    {"page.html": "{{ missing }}"},
    {"page.html": "{% block a %}"},
    {"page.html": "{% frobnicate %}"},
    {"base.html": "{% block a %}{% endblock %}", "page.html": '{% extends "base.html" %}\nstray text\n'},
    {"page.html": "{% block a %}{% endblock %}{% block a %}{% endblock %}"},
    {"page.html": '{% include "../outside.html" %}'},
])
def test_mistakes_fail_loudly(tmp_path, files):
    env = environment(tmp_path / "templates", files)
    with pytest.raises((site_templates.TemplateError, FileNotFoundError)):
        env.render("page.html")

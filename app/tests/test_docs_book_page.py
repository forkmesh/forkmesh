import json
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import dashboard_bundle  # noqa: E402
import dashboard_shell  # noqa: E402
import static_routes  # noqa: E402


PUBLIC = ROOT / "public"
DASHBOARD = PUBLIC / "dashboard"
DOCS_JS = (DASHBOARD / "js" / "09-docs.js").read_text(encoding="utf-8")
BOOT_JS = (DASHBOARD / "js" / "10-boot.js").read_text(encoding="utf-8")
SIDEBAR = (DASHBOARD / "partials" / "sidebar.html").read_text(encoding="utf-8")
CHUNK = (PUBLIC / "dashboard-page-docs.js").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
WRANGLER = tomllib.loads((ROOT / "wrangler.toml").read_text(encoding="utf-8"))


def function_source(name):
    match = re.search(
        r"^  (?:async )?function %s\(.*?(?=^  (?:async )?function |\Z)" % re.escape(name),
        DOCS_JS, re.M | re.S)
    assert match, name
    return match.group(0)


def test_docs_page_is_a_composed_dashboard_page():
    page = dashboard_shell.PAGES["docs"]
    assert page["route"] == "/dashboard/docs"
    assert page["asset"] == "dashboard/docs/index.html"
    assert static_routes.DASHBOARD_PAGE_ASSETS["/dashboard/docs"] == page["asset"]
    assert "/dashboard/docs/index.html" in static_routes.BLOCKED_STATIC_HTML_PATHS
    first = WRANGLER["assets"]["run_worker_first"]
    assert "!/dashboard/docs" in first and "!/dashboard/docs/" in first
    built = (PUBLIC / page["asset"]).read_text(encoding="utf-8")
    assert built.count("<section data-view=") == 1
    assert 'data-page="docs"' in built
    for marker in ("data-docs-list", "data-docs-menu", "data-docs-filter",
                   "data-docs-contents", "data-docs-notice"):
        assert marker in built, marker
    assert "/dashboard-page-docs.js" in built


def test_page_script_is_a_lazy_chunk_wired_to_boot():
    assert dashboard_bundle.LAZY_CHUNKS["docs"] == "09-docs.js"
    assert "09-docs.js" in dashboard_bundle.FRAGMENTS
    assert dashboard_shell.CHUNK_PAGES["docs"] == "docs"
    assert "dashboard-page-docs.js" in dashboard_shell.CACHE_BUSTED_ASSETS
    assert '"docs": initDocsPage,' in BOOT_JS
    assert "function initDocsPage()" in CHUNK


def test_sidebar_links_the_page_for_everyone():
    link = SIDEBAR[SIDEBAR.index('data-nav="docs"'):]
    tag = link[:link.index(">")]
    assert 'href="/dashboard/docs"' in tag
    assert "data-admin-only" not in tag
    assert link[:link.index("</a>")].endswith("Developer docs")


def test_api_is_routed_and_edits_are_admin_only():
    assert '"/api/docs-book"' in RUNTIME
    assert "docs_book.route(self.env, request, url.path)" in RUNTIME
    save = function_source("docsBookSave")
    assert '"/api/docs-book/save"' in save
    assert "oid: doc.oid" in save
    request = function_source("docsBookRequest")
    assert 'credentials: "same-origin"' in request
    separator = function_source("docsBookSeparator")
    assert "docsBook.canEdit" in separator
    assert "docsBookDateLabel(doc)" in separator
    menu = function_source("docsBookShowMenu")
    assert "if (docsBook.canEdit && !doc.truncated)" in menu
    assert "Delete section" in menu
    assert "Select section" in menu


def test_repository_text_reaches_the_dom_as_text_or_through_the_renderer():
    for match in re.finditer(r"\.innerHTML\s*=\s*([^;]+);", DOCS_JS):
        assert match.group(1).startswith("renderMarkdown("), match.group(0)
    assert "insertAdjacentHTML" not in DOCS_JS


NODE = shutil.which("node")


@pytest.mark.skipif(not NODE, reason="node is not installed")
def test_blocks_sections_and_deletion_keep_the_rest_of_the_file():
    source = "\n".join([
        "# Title", "", "Intro line one", "intro line two.", "",
        "## First", "", "- a", "- b", "", "```", "code", "", "more", "```",
        "", "## Second", "", "Text.", "### Deep", "Deeper.", "",
        "## Third", "", "End.", "",
    ])
    script = "\n".join([
        function_source("docsBookBlocks"),
        function_source("docsBookSection"),
        function_source("docsBookWithout"),
        function_source("docsBookSourceRange"),
        "const source = %s;" % json.dumps(source),
        "const doc = { content: source, ...docsBookBlocks(source) };",
        "const second = doc.blocks.findIndex((b) => b.title === 'Second');",
        "const section = docsBookSection(doc, second + 1);",
        "const code = doc.blocks.findIndex((b) => doc.lines[b.start] === '```');",
        "const range = docsBookSourceRange(doc, [code]);",
        "console.log(JSON.stringify({",
        "  blocks: doc.blocks.map((b) => [b.start, b.end, b.level]),",
        "  section: section.title,",
        "  withoutSection: docsBookWithout(doc, section.indexes),",
        "  withoutFirstList: docsBookWithout(doc, [3]),",
        "  code: source.slice(range.start, range.end),",
        "}));",
    ])
    result = json.loads(subprocess.run(
        [NODE, "-e", script], capture_output=True, text=True, check=True,
    ).stdout)
    assert result["blocks"] == [
        [0, 1, 1], [2, 4, 0], [5, 6, 2], [7, 9, 0], [10, 15, 0],
        [16, 17, 2], [18, 19, 0], [19, 20, 3], [20, 21, 0], [22, 23, 2],
        [24, 25, 0],
    ]
    assert result["section"] == "Second"
    assert result["withoutSection"] == "\n".join([
        "# Title", "", "Intro line one", "intro line two.", "",
        "## First", "", "- a", "- b", "", "```", "code", "", "more", "```",
        "", "## Third", "", "End.", "",
    ])
    assert "- a" not in result["withoutFirstList"]
    assert "## First\n\n```" in result["withoutFirstList"]
    assert result["code"] == "```\ncode\n\nmore\n```"

#!/usr/bin/env python3
"""Site-wide contract for deploy-only content-hash cache busters.

All checked-in App, WWW, docs/blog, and World pages link first-party assets with
the shared ``?v=${version}`` placeholder. ``build_site_assets.py`` copies those
sources into each worker's ``dist`` tree, hashes the assets actually present
there, and resolves that placeholder in HTML and dynamic JavaScript loaders.
"""

import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
PUBLIC_ROOTS = (
    ROOT / "public",
    REPOSITORY / "www" / "public",
    REPOSITORY / "world" / "public",
)
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import dashboard_shell  # noqa: E402

TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import docs_site  # noqa: E402


REFERENCE_RE = re.compile(
    r'(?:src|href)="/([\w.-]+\.(?:js|css))(\?v=([^"]*))?"'
)

# Served from public/ but deliberately unversioned.
UNVERSIONED_ASSETS = {
    # Consent controller; it loads the tracking SDK itself, only after opt-in.
    "analytics.js",
    # ES module entry. Its static imports carry their own URLs, so stamping only
    # the entry would not invalidate the rest of its module graph.
    "chat.js",
}


def _documents():
    return tuple(
        path
        for root in PUBLIC_ROOTS
        for path in sorted(root.rglob("*.html"))
    )


def _sources():
    """Authored HTML plus the templates that generate HTML at staging time."""
    for path in _documents():
        yield path, path.read_text(encoding="utf-8")
    # Rendered documentation pages (docs_site.stage_docs) are written into
    # dist before the stamper runs, so a rendered page carries the placeholder.
    catalog = docs_site.load_catalog()
    yield ROOT / "tools" / "docs_site.py", docs_site.page_html(
        catalog, catalog.documents[0]
    )


def test_every_cache_busted_source_reference_uses_version_placeholder():
    seen = set()
    for path, html in _sources():
        for name, query, _ in REFERENCE_RE.findall(html):
            if name in UNVERSIONED_ASSETS:
                assert not query, (path, name)
                continue
            if name in dashboard_shell.CACHE_BUSTED_ASSETS:
                assert query == "?v=${version}", (path, name, query)
                seen.add(name)
                continue
            # Other stylesheets are stable authored design assets. JavaScript
            # must be explicitly classified so a new executable bundle cannot
            # accidentally bypass the cache-busting manifest.
            assert not query and name.endswith(".css"), (path, name)
    assert seen == set(dashboard_shell.CACHE_BUSTED_ASSETS)


def test_stamper_versions_html_and_dynamic_loaders_from_one_map():
    versions = {"api-client.js": "abc123", "chat.css": "def456",
                "dashboard-chat.js": "789abc"}
    html = (
        '<script src="/api-client.js?v=${version}"></script>'
        '<link rel="stylesheet" href="/chat.css?v=${version}">'
    )
    source = (
        'const version = "__FORKMESH_ASSET_VERSION__";\n'
        'script.src = `/dashboard-chat.js?v=${version}`;'
    )
    assert dashboard_shell.stamp_asset_versions(html, versions) == (
        '<script src="/api-client.js?v=abc123"></script>'
        '<link rel="stylesheet" href="/chat.css?v=def456">'
    )
    assert dashboard_shell.stamp_js_asset_versions(source, versions) == (
        'const version = "789abc";\n'
        'script.src = `/dashboard-chat.js?v=${version}`;'
    )


def test_each_worker_build_stages_versions_after_source_generation():
    commands = {
        unit: tomllib.loads(
            (REPOSITORY / unit / "wrangler.toml").read_text(encoding="utf-8")
        )["build"]["command"]
        for unit in ("app", "www", "world")
    }
    assert "build_site_assets.py app" in commands["app"]
    assert "build_site_assets.py www" in commands["www"]
    assert "build_site_assets.py world" in commands["world"]
    assert commands["app"].index("build_dashboard_assets.py") < \
        commands["app"].index("build_site_assets.py")
    assert "build_dashboard_assets.py" not in commands["www"]

#!/usr/bin/env python3

import re
import tomllib
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
WWW_PUBLIC = REPOSITORY / "www" / "public"
PAGE = (WWW_PUBLIC / "ecosystem.html").read_text(encoding="utf-8")
INDEX = (WWW_PUBLIC / "index.html").read_text(encoding="utf-8")


def _landing_section():
    start = INDEX.index('<section id="ecosystem"')
    return INDEX[start:INDEX.index("</section>", start)]


def _scripts(document):
    return re.findall(r"<script>(.*?)</script>", document, re.DOTALL)


def test_the_page_uses_the_shared_site_chrome_and_metadata():
    assert "<title>ForkMesh Ecosystem</title>" in PAGE
    assert '<link rel="canonical" href="https://forkmesh.com/ecosystem" />' in PAGE
    assert '<meta property="og:url" content="https://forkmesh.com/ecosystem" />' in PAGE
    body = PAGE[PAGE.index("<body"):]
    assert '<div data-forkmesh-header="simple"></div>' in body
    assert '<div data-forkmesh-footer="standard"></div>' in body
    for asset in ("/styles.css", "/site-header.js?v=${version}",
                  "/site-footer.js?v=${version}", "/api-client.js?v=${version}"):
        assert asset in PAGE


def test_the_page_reads_the_public_api_and_builds_nodes_safely():
    script = _scripts(PAGE)[-1]
    assert 'const API = "/api/ecosystem";' in script
    assert 'credentials: "omit"' in script
    assert "innerHTML" not in script
    assert "insertAdjacentHTML" not in script
    assert 'url.protocol === "https:" || url.protocol === "http:"' in script
    assert '/^\\/ecosystem\\/([a-z0-9][a-z0-9-]{0,63})\\/?$/' in script
    for label in ("ForkMesh relationship", "What it does",
                  "Why we're watching", "ForkMesh integrations",
                  "Last reviewed: ", '"Compatible"', '"Integration"'):
        assert label in script


def test_the_landing_page_has_the_ecosystem_section_before_support():
    everywhere = INDEX.index('<section id="everywhere"')
    ecosystem = INDEX.index('<section id="ecosystem"')
    support = INDEX.index('<section id="support-forkmesh"')
    assert everywhere < ecosystem < support
    assert '</section>\n\n      <section id="support-forkmesh"' in INDEX
    section = _landing_section()
    assert '<h2 id="ecosystem-title">ForkMesh Ecosystem</h2>' in section
    assert 'href="/ecosystem"' in section
    assert "mailto:founders@forkmesh.com?subject=ForkMesh%20Ecosystem%20listing" in section
    script = next(block for block in _scripts(INDEX)
                  if "[data-eco-section]" in block)
    assert 'fetch("/api/ecosystem", { headers: { accept: "application/json" }, credentials: "omit" })' in script
    assert "innerHTML" not in script
    assert "section.hidden = true;" in script


def test_every_navigation_links_to_the_ecosystem():
    assert '<a class="home-menu-item" href="/ecosystem">' in INDEX
    assert '<a href="/ecosystem">Ecosystem</a>' in INDEX
    hackathon = (WWW_PUBLIC / "hackathon.html").read_text(encoding="utf-8")
    assert '<a class="home-menu-item" href="/ecosystem">' in hackathon
    assert '<a href="/ecosystem">Ecosystem</a>' in hackathon
    header = (WWW_PUBLIC / "site-header.js").read_text(encoding="utf-8")
    assert '<a href="/ecosystem">Ecosystem</a>' in header
    footer = (WWW_PUBLIC / "site-footer.js").read_text(encoding="utf-8")
    assert '{ href: "/ecosystem", label: "Ecosystem" }' in footer


def test_every_worker_runs_first_for_ecosystem_urls():
    for unit in ("www", "app", "world"):
        manifest = tomllib.loads(
            (REPOSITORY / unit / "wrangler.toml").read_text(encoding="utf-8"))
        routes = manifest["assets"]["run_worker_first"]
        assert "/ecosystem" in routes, unit
        assert "/ecosystem/*" in routes, unit
    bootstrap = (REPOSITORY / "tools" / "cloudflare_bootstrap.py").read_text(
        encoding="utf-8")
    assert "'  \"/ecosystem\",'" in bootstrap
    assert "'  \"/ecosystem/*\",'" in bootstrap


def test_the_directory_has_a_sortable_spreadsheet_list_view():
    assert 'data-eco-view="cards" aria-pressed="true"' in PAGE
    assert 'data-eco-view="list" aria-pressed="false"' in PAGE
    assert 'data-eco-filter="usedBy"' in PAGE
    assert "data-eco-csv hidden" in PAGE
    script = _scripts(PAGE)[-1]
    for label in ('"Name"', '"Category"', '"Maker"', '"License"',
                  '"Relationship"', '"In ForkMesh"', '"Website"'):
        assert label in script
    assert 'next.set("view", "list")' in script
    assert 'next.set("uses", "1")' in script
    assert 'th.setAttribute("aria-sort"' in script
    assert 'project.integration !== "uses"' in script
    assert 'text = "\'" + text' in script
    assert "How ForkMesh uses it" in script

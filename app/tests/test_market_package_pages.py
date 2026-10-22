#!/usr/bin/env python3

import html
import json
import re
import sys
from pathlib import Path

import pytest


REPOSITORY = Path(__file__).resolve().parents[2]
TOOLS = REPOSITORY / "app" / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_market_catalog  # noqa: E402


PAGE_LINK_RE = re.compile(r'href="/market/([^"#?]+)"')


def catalog_ids():
    catalog = json.loads(
        (REPOSITORY / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    return [entry["id"] for entry in catalog["packages"]]


@pytest.fixture(scope="module")
def pages():
    return build_market_catalog.package_pages()


def test_every_catalog_package_gets_its_own_page(pages):
    assert sorted(pages) == sorted(catalog_ids())
    for manifest in build_market_catalog.load_packages():
        page = pages[manifest["id"]]
        name = html.escape(manifest["name"], quote=True)
        assert f"<h1>{name} " in page, manifest["id"]
        assert html.escape(manifest["summary"], quote=True) in page, manifest["id"]
        assert (f'<link rel="canonical" href="https://forkmesh.com/market/{manifest["id"]}" />'
                in page), manifest["id"]
        assert f'data-market-package-page="{manifest["id"]}"' in page
        assert f'data-market-rating="{manifest["id"]}"' in page
        assert "/market-package.js?v=" in page and "/market-package.css?v=" in page
        assert '"@type":"SoftwareApplication"' in page
        assert "{{" not in page and "{%" not in page


def test_package_pages_link_only_to_real_packages(pages):
    ids = set(catalog_ids())
    for package_id, page in pages.items():
        for target in PAGE_LINK_RE.findall(page):
            assert target in ids or target.startswith("previews/"), (package_id, target)


def test_parent_pages_list_the_packages_they_include(pages):
    workflows = pages["workflows"]
    assert "Includes 14 packages" in workflows
    assert 'href="/market/workflows-pack-communication"' in workflows
    assert 'href="/market/workflows"' in pages["workflows-pack-ai"]
    assert "Part of" in pages["workflows-pack-ai"]


def test_install_buttons_match_how_each_package_installs(pages):
    assert build_market_catalog.INSTALL_URL + "vault" in pages["vault"]
    assert "?install=" not in pages["crm"]
    assert "installed for an organization" in pages["crm"]
    assert "?install=" not in pages["gemma-4"]
    assert "desktop app and search for <strong>Gemma 4</strong>" in pages["gemma-4"]
    assert "?install=" not in pages["sign-in"]
    assert 'href="/mobile"' in pages["sign-in"]


def test_readmes_render_with_links_that_leave_the_package_folder(pages):
    assert 'id="mp-readme-title"' in pages["strata"]
    assert "<h1" not in pages["strata"].split('id="mp-readme-title"', 1)[1]
    link = build_market_catalog.readme_link
    tree = build_market_catalog.REPOSITORY_URL
    assert link("packages/vault", "../../../docs/marketplace/plugins.md", False) == (
        tree + "/docs/marketplace/plugins.md")
    assert link("packages/vault", "content/README.md#use", False) == (
        tree + "/marketplace/packages/vault/content/README.md#use")
    assert link("packages/vault", "preview/market.svg", True) == (
        "/market/previews/packages/vault/preview/market.svg")
    for target in ("#section", "https://example.com/x", "mailto:a@example.com"):
        assert link("packages/vault", target, False) == target


def test_staging_writes_one_page_per_package(tmp_path):
    written = build_market_catalog.stage_package_pages(tmp_path)
    assert len(written) == len(catalog_ids())
    assert (tmp_path / "market" / "nemotron-3.5-lightning.html").is_file()
    with pytest.raises(RuntimeError):
        build_market_catalog.stage_package_pages(tmp_path)


def test_market_cards_link_to_their_pages():
    page = (REPOSITORY / "www" / "templates" / "pages" / "market.html").read_text(
        encoding="utf-8")
    for package_id in catalog_ids():
        assert f'id="package-{package_id}"' in page, package_id
        assert f'href="/market/{package_id}"' in page, package_id


def test_features_plugin_tags_open_package_pages():
    page = (REPOSITORY / "www" / "templates" / "pages" / "features.html").read_text(
        encoding="utf-8")
    ids = set(catalog_ids())
    targets = PAGE_LINK_RE.findall(page)
    assert targets
    assert set(targets) <= ids
    assert page.count('class="fx-tag fx-tag-plugin" href="/market/') >= 100


def test_dashboard_installs_one_package_from_a_link():
    source = (REPOSITORY / "app" / "public" / "dashboard" / "js" / "09-apps.js").read_text(
        encoding="utf-8")
    assert 'params.get("install")' in source
    assert ".then(runAppsInstallAllRequest).then(runAppsInstallRequest)" in source
    bundle = (REPOSITORY / "app" / "public" / "dashboard-page-apps.js").read_text(
        encoding="utf-8")
    assert "runAppsInstallRequest" in bundle

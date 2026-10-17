#!/usr/bin/env python3

import json
import sys
import tomllib
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
SRC = REPOSITORY / "app" / "src"
TOOLS = REPOSITORY / "app" / "tools"
for path in (SRC, TOOLS):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import build_market_catalog  # noqa: E402
import market_packages  # noqa: E402
import static_routes  # noqa: E402


def catalog_ids():
    catalog = json.loads(
        (REPOSITORY / "marketplace" / "catalog.json").read_text(encoding="utf-8"))
    return [entry["id"] for entry in catalog.get("packages", catalog)]


def test_plugin_share_links_open_the_market_document():
    assert static_routes.external_site_route("/market/crm") == "www"
    assert static_routes.external_site_route(
        "/market/nemotron-3.5-lightning/") == "www"
    assert static_routes.site_document_asset_path("/market/crm/") == "/market"
    assert static_routes.site_document_asset_path("/market") == "/market"
    for path in ("/market/Crm", "/market/crm/settings", "/market/previews",
                 "/market/previews/packages/notes/preview/market.webp"):
        assert static_routes.external_site_route(path) is None, path
        assert static_routes.site_document_asset_path(path) == path
    assert not static_routes.looks_like_repo_route("/market/crm")


def test_every_catalog_package_has_a_share_link():
    ids = catalog_ids()
    assert ids
    for package_id in ids:
        assert market_packages.ID_RE.match(package_id), package_id
        assert static_routes.is_market_package_path("/market/" + package_id), package_id
        assert build_market_catalog.share_url(package_id) == (
            "https://forkmesh.com/market/" + package_id)
    assert "previews" not in ids


def test_market_cards_offer_share_and_copy_link():
    page = (REPOSITORY / "www" / "templates" / "pages" / "market.html").read_text(
        encoding="utf-8")
    for package_id in ("crm", "notes"):
        url = build_market_catalog.share_url(package_id)
        assert f'data-market-share="{url}"' in page
        assert f'data-market-copy-link="{url}"' in page
    assert "navigator.share" in page
    assert 'card.classList.add("is-shared")' in page


def test_every_worker_runs_first_for_plugin_share_links():
    for unit, name in (("www", "wrangler.toml"), ("www", "wrangler.production.toml"),
                       ("app", "wrangler.toml"), ("world", "wrangler.toml")):
        manifest = tomllib.loads(
            (REPOSITORY / unit / name).read_text(encoding="utf-8"))
        routes = manifest["assets"]["run_worker_first"]
        assert "/market/*" in routes, (unit, name)
        assert "!/market/previews/*" in routes, (unit, name)
    bootstrap = (REPOSITORY / "tools" / "cloudflare_bootstrap.py").read_text(
        encoding="utf-8")
    assert "'  \"/market/*\",'" in bootstrap
    assert "'  \"!/market/previews/*\",'" in bootstrap


def test_dashboard_plugin_menu_shares_and_copies_the_link():
    header = (REPOSITORY / "app" / "public" / "dashboard" / "partials" / "header.html"
              ).read_text(encoding="utf-8")
    assert "data-plugin-menu-share" in header
    assert "data-plugin-menu-copy-link" in header
    menu = (REPOSITORY / "app" / "public" / "dashboard" / "js" / "09-plugin-menu.js"
            ).read_text(encoding="utf-8")
    assert "https://forkmesh.com/market/${encodeURIComponent(id)}" in menu
    assert "navigator.share({" in menu
    assert "copyTextToClipboard(pluginShareUrl(plugin.id))" in menu

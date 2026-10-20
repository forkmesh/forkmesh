#!/usr/bin/env python3

import json
import sys
from pathlib import Path

import pytest


REPOSITORY = Path(__file__).resolve().parents[2]
APP = REPOSITORY / "app"
for root in (REPOSITORY / "marketplace" / "tools", APP / "tools", APP / "src"):
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))

from marketplace_lib import MarketplaceError  # noqa: E402
import build_app_plugins  # noqa: E402
import static_routes  # noqa: E402


def _fixture(root: Path, route: str = "/handbook", bundled: bool = True) -> Path:
    package = root / "packages" / "handbook"
    web = package / "content" / "www"
    web.mkdir(parents=True)
    (package / "preview").mkdir()
    (package / "preview" / "market.png").write_bytes(b"png")
    (web / "page.html").write_text(
        '<link rel="stylesheet" href="/handbook/page.css?v=${version}" />\n'
        '<script src="/handbook/page.js?v=${version}" defer></script>\n',
        encoding="utf-8",
    )
    (web / "page.js").write_text('fetch("/handbook/data.json?v=${version}");\n', encoding="utf-8")
    (web / "page.css").write_text("body { color: red; }\n", encoding="utf-8")
    (web / "builder.py").write_text(
        "import json\n"
        "def build(repository, destination):\n"
        "    (destination / 'data.json').write_text(json.dumps({'root': repository.name}))\n",
        encoding="utf-8",
    )
    manifest = {
        "schemaVersion": 1,
        "id": "handbook",
        "name": "Handbook",
        "creator": {"id": "did:forkmesh:tester", "handle": "tester",
                    "displayName": "Tester", "verified": False},
        "type": "extension.native",
        "version": "1.0.0",
        "summary": "A public page staged on the website from a package.",
        "license": {"model": "free-open-source", "spdx": "MIT",
                    "creatorRetainsOwnership": True},
        "price": {"amount": 0, "currency": "USD"},
        "entrypoints": {
            "settings": "builtin:package",
            "page": "content/www/page.html",
            "script": "content/www/page.js",
            "style": "content/www/page.css",
            "builder": "content/www/builder.py",
        },
        "contributions": [{
            "socket": "www.page",
            "entrypoint": "page",
            "config": {"route": route, "title": "Handbook",
                       "assets": ["script", "style"], "builder": "builder"},
        }],
        "permissions": [],
        "screenshots": [{"path": "preview/market.png", "alt": "Handbook"}],
        "dependencies": [],
        "install": {"bundled": bundled, "default": True},
        "lifecycle": {"migrations": []},
    }
    (package / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    catalog = root / "catalog.json"
    catalog.write_text(json.dumps({
        "schemaVersion": 2,
        "marketplace": {"id": "test", "name": "Test", "locations": [
            {"transport": "local", "url": ".", "catalog": "catalog.json"}]},
        "packages": [{"id": "handbook", "version": "1.0.0",
                      "manifest": "packages/handbook/manifest.json"}],
    }), encoding="utf-8")
    (root / "plugins.json").write_text(
        json.dumps({"schemaVersion": 1, "plugins": ["handbook"]}), encoding="utf-8")
    return catalog


def test_a_package_page_is_staged_with_its_assets_and_built_data(tmp_path):
    catalog = _fixture(tmp_path / "market")
    dist = tmp_path / "dist"
    dist.mkdir()
    assert build_app_plugins.stage_www_pages(dist, catalog) == ["/handbook"]
    page = (dist / "handbook.html").read_text(encoding="utf-8")
    script = (dist / "handbook" / "page.js").read_text(encoding="utf-8")
    assert json.loads((dist / "handbook" / "data.json").read_text())["root"] == REPOSITORY.name
    assert "${version}" not in page and "${version}" not in script
    assert "/handbook/data.json?v=" in script
    assert "/handbook/page.js?v=" in page and "/handbook/page.css?v=" in page


def test_the_script_version_follows_the_data_it_loads(tmp_path):
    catalog = _fixture(tmp_path / "market")
    first, second = tmp_path / "one", tmp_path / "two"
    first.mkdir()
    second.mkdir()
    build_app_plugins.stage_www_pages(first, catalog)
    builder = tmp_path / "market/packages/handbook/content/www/builder.py"
    builder.write_text(builder.read_text().replace("'root'", "'top'"), encoding="utf-8")
    build_app_plugins.stage_www_pages(second, catalog)
    version = lambda root: (root / "handbook.html").read_text().split("page.js?v=")[1][:12]
    assert version(first) != version(second)


@pytest.mark.parametrize("route", ["/market", "/dashboard", "/api", "/Design", "/a/b", "design", "/index"])
def test_a_package_cannot_take_a_core_or_invalid_route(tmp_path, route):
    catalog = _fixture(tmp_path / "market", route=route)
    with pytest.raises(MarketplaceError):
        build_app_plugins.www_entries(catalog)


def test_an_unbundled_package_stages_nothing(tmp_path):
    catalog = _fixture(tmp_path / "market", bundled=False)
    dist = tmp_path / "dist"
    dist.mkdir()
    assert build_app_plugins.stage_www_pages(dist, catalog) == []
    assert not (dist / "handbook.html").exists()
    namespace = {}
    exec(build_app_plugins.registry_source(catalog), namespace)
    assert namespace["PLUGIN_WWW_SITE_ROUTES"] == frozenset()


def test_the_generated_registry_hands_the_route_to_the_website(tmp_path):
    catalog = _fixture(tmp_path / "market")
    namespace = {}
    exec(build_app_plugins.registry_source(catalog), namespace)
    assert namespace["PLUGIN_WWW_SITE_ROUTES"] == frozenset({"handbook"})


def test_the_design_system_owns_design_in_the_real_catalog():
    assert build_app_plugins.www_routes() == ("design",)
    assert "design" not in static_routes.CORE_RESERVED_ROUTE_PREFIXES
    assert static_routes.external_site_route("/design") is None


def test_the_website_build_stages_package_pages():
    source = (APP / "tools" / "build_site_assets.py").read_text(encoding="utf-8")
    stage = source[source.index("def stage_www"):source.index("def stage_app")]
    assert stage.index("stage_www_pages") < stage.index("stamp_canonical_links")
    assert stage.index("stage_www_pages") < stage.index("build_www_sitemap")

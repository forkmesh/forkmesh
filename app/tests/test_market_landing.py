#!/usr/bin/env python3
"""Static contracts for the public /market landing page.

The Market's shelves are only credible if the public page shows what actually
ships, so the package cards are generated from ``marketplace/catalog.json`` and
these tests fail when the committed page drifts from the catalog, the package
type enum, or the licence models a creator may choose.
"""

import json
import sys
import tomllib
from pathlib import Path


APP = Path(__file__).resolve().parents[1]
REPOSITORY = APP.parent
WWW_PUBLIC = REPOSITORY / "www" / "public"
MARKETPLACE = REPOSITORY / "marketplace"
PAGE = WWW_PUBLIC / "market.html"

for extra in (APP / "src", APP / "tools"):
    if str(extra) not in sys.path:
        sys.path.insert(0, str(extra))

import build_market_catalog  # noqa: E402
import static_routes  # noqa: E402

MARKET_HTML = PAGE.read_text(encoding="utf-8")
SCHEMA = json.loads((MARKETPLACE / "manifest.schema.json").read_text(encoding="utf-8"))
AGENT_SCHEMA = json.loads(
    (MARKETPLACE / "agent.schema.json").read_text(encoding="utf-8")
)
CATALOG = json.loads((MARKETPLACE / "catalog.json").read_text(encoding="utf-8"))


def _manifests():
    return [
        json.loads((MARKETPLACE / entry["manifest"]).read_text(encoding="utf-8"))
        for entry in CATALOG["packages"]
    ]


def _nested_ids():
    ids = {manifest["id"] for manifest in _manifests()}
    return {
        manifest["id"]
        for manifest in _manifests()
        if build_market_catalog.market_parent(manifest, ids)
    }


def test_market_page_lists_every_catalogued_package():
    manifests = _manifests()
    assert manifests, "the marketplace catalog is empty"
    nested = _nested_ids()
    # The generator HTML-escapes every manifest string (an apostrophe in a
    # summary becomes &#x27;), so compare what the page can actually contain.
    shown = build_market_catalog.escape
    for manifest in manifests:
        assert f'data-market-package="{manifest["id"]}"' in MARKET_HTML
        assert shown(manifest["name"]) in MARKET_HTML
        assert shown(manifest["summary"]) in MARKET_HTML
        assert f'v{manifest["version"]}' in MARKET_HTML
        directory = next(
            entry["manifest"].rsplit("/", 1)[0]
            for entry in CATALOG["packages"] if entry["id"] == manifest["id"]
        )
        assert (
            f'<a class="market-repository" '
            f'href="{build_market_catalog.SOURCE_URL}/{directory}">'
            "View public repository &nearr;</a>"
        ) in MARKET_HTML
        if manifest["id"] in nested:
            continue
        assert f'data-market-rating="{manifest["id"]}"' in MARKET_HTML
        for permission in manifest["permissions"]:
            assert shown(permission["label"]) in MARKET_HTML


def test_market_page_shows_every_manifest_screenshot():
    nested = _nested_ids()
    for entry, manifest in zip(CATALOG["packages"], _manifests()):
        if manifest["id"] in nested:
            continue
        directory = entry["manifest"].rsplit("/", 1)[0]
        for screenshot in manifest["screenshots"]:
            url = build_market_catalog.screenshot_url(directory, screenshot["path"])
            preview = (
                '<img class="market-screenshot" '
                f'src="{build_market_catalog.escape(url)}" '
                f'alt="{build_market_catalog.escape(screenshot["alt"])}" '
                'loading="lazy" decoding="async" />'
            )
            assert preview in MARKET_HTML


def test_status_checks_are_nested_inside_status_page():
    nested = _nested_ids()
    checks = {
        entry["id"] for entry in CATALOG["packages"]
        if entry["id"].startswith("status-check-")
    }
    assert checks and checks <= nested
    start = MARKET_HTML.index('<article class="market-item" data-market-package="status-page"')
    card = MARKET_HTML[start:MARKET_HTML.index("</article>", start)]
    assert f"Includes {len(checks)} packages, added and removed with it" in card
    for check in checks:
        assert f'<li class="market-nested-item" data-market-package="{check}">' in card
        assert f'<article class="market-item" data-market-package="{check}"' not in MARKET_HTML
    assert MARKET_HTML.count('<article class="market-item"') == len(CATALOG["packages"]) - len(nested)


def test_market_catalog_block_is_regenerated_from_the_catalog():
    assert build_market_catalog.START in MARKET_HTML
    assert build_market_catalog.END in MARKET_HTML
    assert build_market_catalog.rewrite(MARKET_HTML) == MARKET_HTML, (
        "www/public/market.html is stale — run "
        "python3 app/tools/build_market_catalog.py"
    )


def _card(package_id):
    start = MARKET_HTML.index(f'<article class="market-item" data-market-package="{package_id}"')
    return MARKET_HTML[start:MARKET_HTML.index("</article>", start)]


def test_market_page_says_where_account_packages_show():
    assert "<dt>Shows on</dt>\n            <dd>Phone only</dd>" in _card("food-guide")
    assert "<dd>Phone, Desktop and Web; you choose where</dd>" in _card("workflows")
    for manifest in _manifests():
        if (manifest.get("install") or {}).get("sync") != "account":
            if manifest["id"] not in _nested_ids():
                assert "<dt>Shows on</dt>" not in _card(manifest["id"]), manifest["id"]
    assert build_market_catalog.shows_on(
        {"install": {"sync": "account", "places": ["desktop", "web"]}}
    ) == "Desktop and Web; you choose where"
    assert build_market_catalog.shows_on({"install": {"sync": "device"}}) == ""


def test_every_package_has_settings_and_agent_descriptors_follow_the_shared_shape():
    assert "settings" in SCHEMA["properties"]["entrypoints"]["required"]
    agent_required = set(AGENT_SCHEMA["required"])
    for entry, manifest in zip(CATALOG["packages"], _manifests()):
        relative = entry["manifest"]
        assert manifest["entrypoints"].get("settings"), manifest["id"]
        agent_path = manifest["entrypoints"].get("agent")
        if not agent_path:
            continue
        descriptor = json.loads(
            (MARKETPLACE / Path(relative).parent / agent_path).read_text(
                encoding="utf-8"
            )
        )
        assert agent_required <= descriptor.keys()
        assert descriptor["defaultModel"] in {
            model["id"] for model in descriptor["models"]
        }


def test_every_package_type_has_a_shelf_on_the_page():
    for package_type in SCHEMA["properties"]["type"]["enum"]:
        assert f'data-package-type="{package_type}"' in MARKET_HTML, (
            f"{package_type} has no shelf on /market"
        )
        assert package_type in build_market_catalog.TYPE_LABELS


def test_creator_guide_covers_the_envelope_and_every_licence_model():
    for model in SCHEMA["properties"]["license"]["properties"]["model"]["enum"]:
        assert f">{model}</code>" in MARKET_HTML, f"{model} is undocumented on /market"
    for risk in SCHEMA["properties"]["permissions"]["items"]["properties"]["risk"]["enum"]:
        assert f">{risk}</code>" in MARKET_HTML
    # The package envelope and the lifecycle contract creators must honour.
    for marker in (
        "manifest.json",
        "content/",
        "preview/",
        "LICENSE",
        "SIGNATURE",
        "creatorRetainsOwnership",
        "revenueSharePercent",
        "marketplace/catalog.json",
        "Read the manifest",
    ):
        assert marker in MARKET_HTML
    assert 'id="create"' in MARKET_HTML
    assert 'id="publish"' in MARKET_HTML


def test_market_page_uses_the_shared_site_chrome():
    assert 'href="/site-header.css"' in MARKET_HTML
    assert 'src="/site-header.js?v=${version}"' in MARKET_HTML
    body = MARKET_HTML[MARKET_HTML.index("<body") :]
    assert '<div data-forkmesh-header="simple"></div>' in body
    assert '<div data-forkmesh-footer="standard"></div>' in body
    assert 'class="site-header"' not in body
    assert 'class="global-nav"' not in body
    assert '<link rel="canonical" href="https://forkmesh.com/market" />' in MARKET_HTML


def test_market_is_a_www_owned_route_on_every_worker():
    assert static_routes.external_site_route("/market") == "www"
    assert "market" in static_routes.RESERVED_ROUTE_PREFIXES
    assert "/market.html" in static_routes.BLOCKED_STATIC_HTML_PATHS
    for unit in ("app", "world"):
        manifest = tomllib.loads(
            (REPOSITORY / unit / "wrangler.toml").read_text(encoding="utf-8")
        )
        assert "/market" in manifest["assets"]["run_worker_first"], unit
    bootstrap = (REPOSITORY / "tools" / "cloudflare_bootstrap.py").read_text(
        encoding="utf-8"
    )
    assert "'  \"/market\",'" in bootstrap


def test_market_is_reachable_from_the_shared_navigation():
    header = (WWW_PUBLIC / "site-header.js").read_text(encoding="utf-8")
    footer = (WWW_PUBLIC / "site-footer.js").read_text(encoding="utf-8")
    assert 'const MARKET_HREF = MARKETING_HOST' in header
    assert ': "https://forkmesh.com/market";' in header
    assert 'href="${MARKET_HREF}">Marketplace ' in header
    assert '{ href: "/market", label: "Market" }' in footer
    redirects = (WWW_PUBLIC / "_redirects").read_text(encoding="utf-8")
    assert "/marketplace /market 308" in redirects


def test_dashboard_sidebar_links_to_the_canonical_marketplace():
    sidebar = (
        REPOSITORY / "app" / "public" / "dashboard" / "partials" /
        "sidebar.html"
    ).read_text(encoding="utf-8")
    assert 'href="https://forkmesh.com/market"' in sidebar
    assert "Marketplace" in sidebar


def test_desktop_publish_button_opens_the_market_guide():
    desktop_market = (REPOSITORY / "desktop" / "src" / "MainWindowMarket.cpp").read_text(
        encoding="utf-8"
    )
    start = desktop_market.index('marketPublishButton')
    end = desktop_market.index('header->addWidget(publish);', start)
    publish_button = desktop_market[start:end]
    assert 'Open the Market publishing instructions' in publish_button
    assert 'QUrl(QStringLiteral("https://forkmesh.com/market"))' in publish_button
    assert "marketplace/README.md" not in publish_button

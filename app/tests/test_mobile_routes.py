#!/usr/bin/env python3

import sys
import tomllib
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
SRC = REPOSITORY / "app" / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import static_routes  # noqa: E402


def test_the_mobile_page_belongs_to_the_marketing_site():
    assert static_routes.external_site_route("/mobile") == "www"
    assert static_routes.external_site_route("/mobile/") == "www"
    assert static_routes.site_document_asset_path("/mobile") == "/mobile"
    assert not static_routes.looks_like_repo_route("/mobile/notes")
    assert "/mobile.html" in static_routes.BLOCKED_STATIC_HTML_PATHS
    assert "mobile" in static_routes.RESERVED_ROUTE_PREFIXES
    assert "mobile" in static_routes.WWW_SITE_ROUTES


def test_every_worker_runs_first_for_the_mobile_page():
    for manifest_path in (
        "www/wrangler.toml",
        "www/wrangler.production.toml",
        "app/wrangler.toml",
        "world/wrangler.toml",
    ):
        manifest = tomllib.loads((REPOSITORY / manifest_path).read_text(encoding="utf-8"))
        assert "/mobile" in manifest["assets"]["run_worker_first"], manifest_path
    bootstrap = (REPOSITORY / "tools" / "cloudflare_bootstrap.py").read_text(encoding="utf-8")
    assert "'  \"/mobile\",'" in bootstrap


def test_the_site_footer_links_the_mobile_page():
    footer = (REPOSITORY / "www" / "public" / "site-footer.js").read_text(encoding="utf-8")
    assert '{ href: "/mobile", label: "Mobile app" }' in footer

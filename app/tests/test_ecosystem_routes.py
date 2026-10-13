#!/usr/bin/env python3

import sys
from pathlib import Path


SRC = Path(__file__).resolve().parents[1] / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import static_routes  # noqa: E402


def test_ecosystem_urls_belong_to_the_marketing_site():
    assert static_routes.external_site_route("/ecosystem") == "www"
    assert static_routes.external_site_route("/ecosystem/radicle") == "www"
    assert static_routes.external_site_route("/ecosystem/Radicle") is None
    assert static_routes.external_site_route("/ecosystem/a/b") is None
    assert not static_routes.looks_like_repo_route("/ecosystem/radicle")
    assert static_routes.site_document_asset_path(
        "/ecosystem/radicle/") == "/ecosystem"
    assert static_routes.site_document_asset_path("/press") == "/press"
    assert "/ecosystem.html" in static_routes.BLOCKED_STATIC_HTML_PATHS
    assert "ecosystem" in static_routes.RESERVED_ROUTE_PREFIXES

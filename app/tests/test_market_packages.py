#!/usr/bin/env python3

import asyncio
import json
import sys
from pathlib import Path


APP = Path(__file__).resolve().parents[1]
SRC = APP / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import market_packages  # noqa: E402


def _response(data, status=200, cache_control=None, extra_headers=None):
    return {"data": data, "status": status, "cache": cache_control,
            "headers": extra_headers or {}}


def _write(root, relative, value):
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def test_real_catalog_lists_every_first_party_package():
    rows = market_packages.catalog_packages()
    catalog = json.loads(
        (market_packages.ROOT / "catalog.json").read_text(encoding="utf-8"))
    assert {row["id"] for row in rows} == {
        entry["id"] for entry in catalog["packages"]}
    crm = next(row for row in rows if row["id"] == "crm")
    assert crm["dataScope"] == "organization"
    assert crm["surfaces"]["web"] == "/dashboard/crm"
    assert crm["surfaces"]["desktop"] is True
    assert crm["api"] == "/api/crm"
    vault = next(row for row in rows if row["id"] == "vault")
    assert vault["dataScope"] == "account"
    ledger = next(row for row in rows if row["id"] == "ledger")
    assert ledger["dataScope"] == ""
    assert ledger["surfaces"]["worker"] is False
    design = next(row for row in rows if row["id"] == "design-system")
    assert design["surfaces"]["web"] == "/design"
    assert design["surfaces"]["desktop"] is True
    assert design["bundled"] is True


def test_rows_are_sorted_and_bounded(tmp_path):
    _write(tmp_path, "catalog.json", {"packages": [
        {"id": "zeta", "manifest": "packages/zeta/manifest.json"},
        {"id": "alpha", "manifest": "packages/alpha/manifest.json"},
        {"id": "dupe", "manifest": "packages/alpha/manifest.json"},
        {"id": "escape", "manifest": "../outside/manifest.json"},
        {"id": "bad", "manifest": "packages/bad/manifest.json"},
    ]})
    _write(tmp_path, "packages/zeta/manifest.json", {
        "id": "zeta", "name": "Zeta", "version": "1.0.0",
        "summary": "x" * 900, "entrypoints": {"desktop": "builtin:zeta"},
        "worker": {"api": "/api/zeta"}, "dataLifecycle": {"scope": "account"},
        "permissions": [{"id": "a", "label": "A", "risk": "weird"}],
        "price": {"amount": 0}})
    _write(tmp_path, "packages/alpha/manifest.json", {
        "id": "alpha", "name": "Alpha", "version": "not-a-version",
        "worker": {"api": "javascript:alert(1)"},
        "contributions": [{"socket": "dashboard.page",
                           "config": {"route": "https://evil.example"}}]})
    _write(tmp_path, "packages/bad/manifest.json", {"id": "Bad Id"})
    rows = market_packages.catalog_packages(tmp_path)
    assert [row["id"] for row in rows] == ["alpha", "zeta"]
    alpha, zeta = rows
    assert alpha["version"] == ""
    assert alpha["api"] == ""
    assert alpha["surfaces"]["web"] == ""
    assert len(zeta["summary"]) == 600
    assert zeta["dataScope"] == "account"
    assert zeta["surfaces"]["worker"] is True
    assert zeta["permissions"] == [{"id": "a", "label": "A", "risk": "sensitive"}]


def test_every_real_package_sends_phones_a_raster_screenshot():
    rows = market_packages.catalog_packages()
    for row in rows:
        assert row["screenshots"], row["id"]
        for shot in row["screenshots"]:
            assert shot["url"].startswith(
                "https://forkmesh.com/market/previews/packages/"), shot
            assert shot["url"].endswith((".png", ".jpg", ".jpeg", ".webp"))
            assert shot["alt"]
            local = market_packages.ROOT / shot["url"].removeprefix(
                market_packages.PREVIEW_ORIGIN)
            assert local.is_file(), local
    notes = next(row for row in rows if row["id"] == "notes")
    assert notes["screenshots"][0]["url"] == (
        "https://forkmesh.com/market/previews/packages/notes/preview/market.webp")


def test_every_svg_preview_has_a_webp_for_phones():
    missing = [
        str(svg.relative_to(market_packages.ROOT))
        for svg in sorted(market_packages.ROOT.glob("packages/*/preview/*.svg"))
        if not svg.with_suffix(".webp").is_file()
    ]
    assert missing == [], (
        "run python3 app/tools/rasterize_market_previews.py: " + ", ".join(missing))


def test_screenshots_skip_unsafe_or_missing_previews(tmp_path):
    _write(tmp_path, "catalog.json", {"packages": [
        {"id": "pics", "manifest": "packages/pics/manifest.json"}]})
    _write(tmp_path, "packages/pics/manifest.json", {
        "id": "pics", "name": "Pics", "screenshots": [
            {"path": "preview/shown.svg", "alt": "Shown " + "x" * 300},
            {"path": "preview/no-raster.svg", "alt": "No raster"},
            {"path": "preview/../../secret.png", "alt": "Escape"},
            {"path": "content/app.png", "alt": "Outside preview"},
            {"path": "preview/missing.png", "alt": "Missing"},
            "not-an-object",
        ]})
    (tmp_path / "packages/pics/preview").mkdir(parents=True)
    for name in ("shown.svg", "shown.webp", "no-raster.svg"):
        (tmp_path / "packages/pics/preview" / name).write_bytes(b"x")
    (tmp_path / "secret.png").write_bytes(b"x")
    (tmp_path / "packages/pics/content").mkdir()
    (tmp_path / "packages/pics/content/app.png").write_bytes(b"x")
    [row] = market_packages.catalog_packages(tmp_path)
    assert row["screenshots"] == [{
        "url": "https://forkmesh.com/market/previews/packages/pics/preview/shown.webp",
        "alt": "Shown " + "x" * 154,
    }]
    assert market_packages.package_row(
        {"id": "pics", "screenshots": [{"path": "preview/shown.png"}]},
    )["screenshots"] == []


def test_handler_answers_get_and_refuses_writes(tmp_path):
    _write(tmp_path, "catalog.json", {"packages": []})
    ok = asyncio.run(market_packages.handle("GET", _response, tmp_path))
    assert ok["status"] == 200
    assert ok["data"]["ok"] is True
    assert ok["data"]["packages"] == []
    assert ok["cache"].startswith("public")
    refused = asyncio.run(market_packages.handle("POST", _response, tmp_path))
    assert refused["status"] == 405
    assert refused["headers"]["allow"] == "GET"


def test_route_is_wired_in_the_app_runtime():
    text = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    assert '"/api/market/packages"' in text
    assert "market_packages.handle(" in text


def test_staged_registry_carries_the_catalog_for_deployed_nodes(monkeypatch):
    tools = APP / "tools"
    if str(tools) not in sys.path:
        sys.path.insert(0, str(tools))
    import build_app_plugins
    namespace = {}
    exec(build_app_plugins.registry_source(), namespace)
    staged = [dict(row) for row in namespace["MARKET_PACKAGES"]]
    assert staged == market_packages.catalog_packages()
    import plugin_registry
    monkeypatch.setattr(plugin_registry, "MARKET_PACKAGES", (
        {"id": "only-staged", "name": "Only staged"},))
    rows = market_packages.payload(APP / "missing-marketplace")["packages"]
    assert rows == []
    assert [row["id"] for row in market_packages.payload()["packages"]] == [
        "only-staged"]


def test_checked_in_registry_stays_empty():
    namespace = {}
    exec((SRC / "plugin_registry.py").read_text(encoding="utf-8"), namespace)
    assert namespace["MARKET_PACKAGES"] == ()
    assert namespace["PLUGIN_WWW_SITE_ROUTES"] == frozenset()


def test_rows_name_the_publisher_and_whether_it_is_verified():
    rows = market_packages.catalog_packages()
    crm = next(row for row in rows if row["id"] == "crm")
    assert crm["publisher"] == "ForkMesh"
    assert crm["verified"] is True
    assert crm["free"] is True
    bare = market_packages.package_row({"id": "bare", "creator": "nobody"})
    assert bare["publisher"] == ""
    assert bare["verified"] is False
    handle = market_packages.package_row(
        {"id": "handle", "creator": {"handle": "acme", "verified": "yes"}})
    assert handle["publisher"] == "acme"
    assert handle["verified"] is False

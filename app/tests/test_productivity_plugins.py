"""Tasks, Calendar, and Notes are Marketplace-owned dashboard plugins."""

import json
import shutil
import sys
from pathlib import Path


APP = Path(__file__).resolve().parents[1]
REPO = APP.parent
for root in (APP / "src", APP / "tools", REPO / "marketplace" / "tools"):
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))

import build_app_plugins  # noqa: E402
import dashboard_shell  # noqa: E402
from marketplace_lib import load_catalog  # noqa: E402


PACKAGES = ("tasks", "calendar", "notes")


def test_productivity_pages_are_catalog_discovered_plugins_not_core_routes():
    catalog = load_catalog(REPO / "marketplace" / "catalog.json")
    assert {entry.package_id for entry in catalog.packages}.issuperset(PACKAGES)
    assert not set(PACKAGES).intersection(dashboard_shell.PAGES)
    entries = {row["item"].package_id: row for row in build_app_plugins._web_entries()}
    for package in PACKAGES:
        row = entries[package]
        assert row["route"] == "/dashboard/" + package
        assert row["script_path"].name == "page.js"
        manifest = json.loads(
            (REPO / "marketplace" / "packages" / package / "manifest.json")
            .read_text(encoding="utf-8")
        )
        assert manifest["install"] == {
            "bundled": True, "default": False, "sync": "account",
            "installedBefore": "2026-10-11"}
        assert manifest["entrypoints"]["desktop"] == "builtin:" + package
        assert manifest["entrypoints"]["worker"] == "builtin:" + package
        assert (REPO / "marketplace" / "packages" / package
                / ".codex-plugin" / "plugin.json").is_file()


def test_productivity_plugin_pages_stage_their_view_script_nav_and_active_state(tmp_path):
    destination = tmp_path / "dist"
    shutil.copytree(APP / "public", destination)
    pages = build_app_plugins.stage_web(destination)
    for package in PACKAGES:
        route = "/dashboard/" + package
        assert pages[route] == "dashboard-plugins/%s/default/index.html" % package
        document = (destination / pages[route]).read_text(encoding="utf-8")
        assert 'data-view="%s"' % package in document
        assert 'data-nav="%s" data-nav-link aria-current="page"' % package in document
        assert '/dashboard-plugins/%s/default/page.js' % package in document
        script = (
            destination / "dashboard-plugins" / package / "default" / "page.js"
        ).read_text(encoding="utf-8")
        assert 'registerPage("%s"' % package in script
        assert 'builtinPage("%s")' % package in script


def test_generated_plugin_browser_assets_bypass_the_repository_worker_router():
    """Plugin JS/registry must be served as assets, not mistaken for repos."""
    import tomllib

    wrangler = tomllib.loads((APP / "wrangler.toml").read_text(encoding="utf-8"))
    routes = wrangler["assets"]["run_worker_first"]
    assert "/*/*" in routes
    assert "!/dashboard-plugins/*" in routes


def test_crm_worker_module_is_present_in_the_deploy_payload():
    payloads = {path.as_posix(): raw for path, _size, raw
                in build_app_plugins.worker_payloads()}
    expected = (REPO / "marketplace" / "packages" / "crm" / "content"
                / "worker" / "crm.py").read_bytes()
    assert payloads["plugins/crm.py"] == expected
    assert "plugins/crm_grants.csv" in payloads
    assert not any(name in payloads for name in ("crm.py", "crm_grants.csv"))
    registry = payloads["plugin_registry.py"].decode("utf-8")
    assert "'crm': {'module': 'crm', 'api': '/api/crm'" in registry


def test_dashboard_boot_has_no_undefined_plugin_registry_and_exposes_bounded_builtins():
    boot = (APP / "public" / "dashboard" / "js" / "10-boot.js").read_text(
        encoding="utf-8")
    assert "...pluginPages" not in boot
    assert 'new Set(["tasks", "calendar", "notes", "workflows"])' in boot
    assert "delete PAGE_INITS[packagedPage]" in boot

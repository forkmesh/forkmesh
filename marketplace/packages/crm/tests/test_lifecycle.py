"""Shared market-plugin lifecycle: install, export-on-uninstall, schema reversal."""

import asyncio
import json
from pathlib import Path
import re
import sys


CRM_PACKAGE = Path(__file__).resolve().parents[1]
REPO = CRM_PACKAGE.parents[2]
ROOT = REPO / "app"
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(CRM_PACKAGE / "content" / "worker"))
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(CRM_PACKAGE / "tests"))
import crm
import plugin_data
import build_app_plugins

_staged_registry = {}
exec(build_app_plugins.registry_source(), _staged_registry)
plugin_data.REGISTRY = _staged_registry["PLUGINS"]

from test_crm import Runtime


def call(runtime, path):
    return asyncio.run(plugin_data.handle(runtime, path))


def crm_call(runtime, path):
    return asyncio.run(crm.handle(runtime, path))


def crm_tables(runtime):
    return sorted(row["name"] for row in runtime.db.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'crm_%'"))


def installs(runtime):
    return runtime.db.execute(
        "SELECT COUNT(*) FROM org_plugin_installs").fetchone()[0]


def test_install_creates_schema_and_uninstall_exports_purges_and_reverses():
    runtime = Runtime(installed=False)
    assert crm_tables(runtime) == []

    state = call(runtime.use("GET"), "/api/plugins/acme")
    entry = next(p for p in state["data"]["plugins"] if p["id"] == "crm")
    assert (entry["installed"], entry["serverData"]) == (False, True)

    installed = call(runtime.use("POST"), "/api/plugins/acme/crm/install")
    assert installed["status"] == 200
    entry = next(p for p in installed["data"]["plugins"] if p["id"] == "crm")
    assert (entry["installed"], entry["installedBy"]) == (True, "alice")
    assert crm_tables(runtime) == [
        "crm_backfills", "crm_contacts", "crm_deck_links", "crm_deck_page_views",
        "crm_deck_sessions", "crm_email_reading", "crm_email_report", "crm_email_sends",
        "crm_email_tracking",
        "crm_list_activity",
        "crm_mail_messages",
        "crm_outreach_log", "crm_outreach_team", "crm_settings",
        "crm_view_shares", "crm_views", "crm_zoho_members"]

    created = crm_call(runtime.use("POST", body={
        "name": "Ada", "email": "ada@example.com"}), "/api/crm/acme/contacts")
    assert created["status"] == 201

    exported = call(runtime.use("GET"), "/api/plugins/acme/crm/export")
    assert exported["data"]["filename"] == "forkmesh-crm-acme.json"
    assert exported["data"]["hasData"] is True
    assert exported["data"]["export"]["contacts"][0]["name"] == "Ada"

    removed = call(runtime.use("POST"), "/api/plugins/acme/crm/uninstall")
    data = removed["data"]
    assert (data["uninstalled"], data["hadData"], data["schemaDropped"]) == (
        True, True, True)
    assert data["export"]["format"] == "forkmesh-crm-export-v1"
    assert data["export"]["contacts"][0]["email"] == "ada@example.com"
    assert crm_tables(runtime) == []
    assert installs(runtime) == 0

    # The CRM API is gated again, and a reinstall starts from a clean slate.
    gated = crm_call(runtime.use("GET"), "/api/crm/acme")
    assert (gated["status"], gated["data"]["error"]) == (
        409, "plugin_not_installed")
    call(runtime.use("POST"), "/api/plugins/acme/crm/install")
    fresh = crm_call(runtime.use("GET"), "/api/crm/acme")
    assert fresh["data"]["contacts"] == []


def test_only_owners_or_admins_manage_the_lifecycle():
    runtime = Runtime(installed=False)
    for action, method in (
            ("install", "POST"), ("uninstall", "POST"), ("export", "GET")):
        denied = call(
            runtime.use(method, "charlie"), f"/api/plugins/acme/crm/{action}")
        assert denied["status"] == 403
    state = call(runtime.use("GET", "charlie"), "/api/plugins/acme")
    assert state["data"]["canManage"] is False
    outsider = call(runtime.use("GET", "bob"), "/api/plugins/acme")
    assert outsider["status"] == 404
    assert crm_tables(runtime) == []


def test_schema_survives_until_the_last_organization_uninstalls():
    runtime = Runtime(installed=False)
    runtime.db.executescript("""
      INSERT INTO orgs VALUES('org:beta','beta','sealed:{}',1);
      INSERT INTO org_members VALUES('org:beta','bi:alice','owner','alice',1);
    """)
    call(runtime.use("POST"), "/api/plugins/acme/crm/install")
    call(runtime.use("POST"), "/api/plugins/beta/crm/install")
    crm_call(runtime.use("POST", body={"name": "Beta Contact"}),
             "/api/crm/beta/contacts")

    first = call(runtime.use("POST"), "/api/plugins/acme/crm/uninstall")
    assert first["data"]["schemaDropped"] is False
    assert crm_tables(runtime) == [
        "crm_backfills", "crm_contacts", "crm_deck_links", "crm_deck_page_views",
        "crm_deck_sessions", "crm_email_reading", "crm_email_report", "crm_email_sends",
        "crm_email_tracking",
        "crm_list_activity",
        "crm_mail_messages",
        "crm_outreach_log", "crm_outreach_team", "crm_settings",
        "crm_view_shares", "crm_views", "crm_zoho_members"]
    still = crm_call(runtime.use("GET"), "/api/crm/beta")
    assert still["data"]["contacts"][0]["name"] == "Beta Contact"

    last = call(runtime.use("POST"), "/api/plugins/beta/crm/uninstall")
    assert last["data"]["schemaDropped"] is True
    assert crm_tables(runtime) == []


def test_install_heals_an_older_installed_plugins_missing_table():
    runtime = Runtime(installed=True)
    runtime.db.execute("DROP TABLE crm_mail_messages")

    # Installing Marketing returns the complete organization plugin state.
    # A CRM install from before crm_mail_messages existed must be upgraded
    # instead of making that unrelated install return a Worker 500.
    installed = call(
        runtime.use("POST"), "/api/plugins/acme/marketing/install")

    assert installed["status"] == 200
    entries = {item["id"]: item for item in installed["data"]["plugins"]}
    assert entries["marketing"]["installed"] is True
    assert entries["crm"]["installed"] is True
    assert "crm_mail_messages" in crm_tables(runtime)


def test_plugins_without_server_data_track_registry_rows_only():
    runtime = Runtime(installed=False)
    installed = call(runtime.use("POST"), "/api/plugins/acme/capture/install")
    entry = next(
        p for p in installed["data"]["plugins"] if p["id"] == "capture")
    assert (entry["installed"], entry["serverData"], entry["hasData"]) == (
        True, False, False)
    no_data = call(runtime.use("GET"), "/api/plugins/acme/capture/export")
    assert (no_data["status"], no_data["data"]["error"]) == (
        409, "plugin_has_no_server_data")
    removed = call(runtime.use("POST"), "/api/plugins/acme/capture/uninstall")
    assert (removed["data"]["hadData"], removed["data"]["export"]) == (
        False, None)
    assert installs(runtime) == 0
    assert crm_tables(runtime) == []


def test_unknown_targets_methods_and_double_uninstall_fail_closed():
    runtime = Runtime(installed=False)
    assert call(
        runtime.use("POST"), "/api/plugins/acme/unknown/install")["status"] == 404
    assert call(
        runtime.use("GET"), "/api/plugins/acme/crm/install")["status"] == 405
    assert call(
        runtime.use("POST"), "/api/plugins/acme/crm/uninstall")["status"] == 409
    assert call(runtime.use("GET"), "/api/plugins/acme/crm/nope")["status"] == 404
    assert call(runtime.use("GET"), "/api/plugins/no-such-org")["status"] == 404


def test_organization_deletion_purges_every_installed_plugin():
    runtime = Runtime(installed=True)
    crm_call(runtime.use("POST", body={"name": "Ada"}), "/api/crm/acme/contacts")
    asyncio.run(plugin_data.purge_org(runtime, "org:acme"))
    assert crm_tables(runtime) == []
    assert installs(runtime) == 0


def test_crm_declares_reversible_organization_data_hooks():
    """The package owns every hook needed to remove its server-side data."""
    manifest = json.loads((CRM_PACKAGE / "manifest.json").read_text())
    assert manifest["dataLifecycle"] == {"scope": "organization"}
    assert manifest["worker"]["module"] == "crm"
    assert plugin_data.REGISTRY["crm"]["organizationData"] is True
    module = plugin_data._data_module("crm")
    for hook in ("DATA_SCHEMA_UP", "DATA_SCHEMA_DOWN", "org_has_data",
                 "export_org_data", "purge_org_data"):
        assert hasattr(module, hook), f"CRM data module missing {hook}"
    created = {match.group(1) for statement in module.DATA_SCHEMA_UP
               for match in [re.search(
                   r"CREATE TABLE IF NOT EXISTS (\w+)", statement)] if match}
    dropped = {match.group(1) for statement in module.DATA_SCHEMA_DOWN
               for match in [re.search(
                   r"DROP TABLE IF EXISTS (\w+)", statement)] if match}
    assert created and created == dropped


def test_generated_plugin_sockets_match_the_packages_exactly():
    """Plugin artifacts are discovered for deploy without entering App source."""
    registry = build_app_plugins.registry_source()
    assert "'crm'" in registry
    assert "'/dashboard/crm'" in registry
    assert "'crm'" not in (ROOT / "src" / "plugin_registry.py").read_text()
    for relative in (
        "public/dashboard/crm/index.html",
        "public/dashboard/js/plugin-crm.js",
        "public/dashboard/partials/plugin-nav.html",
        "public/dashboard/partials/views/plugin-crm.html",
    ):
        assert not (ROOT / relative).exists()


def test_mailtrap_hook_updates_only_the_matching_organization_send():
    source = (CRM_PACKAGE / "content" / "worker" / "crm.py").read_text()
    assert "async def mailtrap_webhook_event" in source
    assert "forkmesh_crm_send_id" in source
    assert "UPDATE crm_email_sends SET status=?,status_at=?" in source
    assert "WHERE send_id=? AND org_bi=?" in source


def test_site_sources_never_name_the_plugin():
    """Ripping a plugin out means deleting its package: the dashboard's own
    sources only expose generic sockets."""
    dashboard = ROOT / "public" / "dashboard"
    sources = [
        dashboard / "partials" / "sidebar.html",
        dashboard / "js" / "01-state.js",
        dashboard / "js" / "10-boot.js",
        ROOT / "src" / "dashboard_bundle.py",
        ROOT / "src" / "dashboard_shell.py",
        ROOT / "src" / "static_routes.py",
        ROOT / "src" / "entry.py",
        ROOT / "src" / "route_handlers.py",
        ROOT / "src" / "plugin_data.py",
        REPO / "desktop" / "CMakeLists.txt",
        REPO / "desktop" / "src" / "MainWindow.h",
        REPO / "desktop" / "src" / "MainWindowChat.cpp",
        REPO / "desktop" / "src" / "MainWindowMarket.cpp",
        REPO / "desktop" / "src" / "NativePackageExtension.cpp",
        REPO / "desktop" / "src" / "NativePackageExtension.h",
    ]
    for path in sources:
        text = path.read_text()
        assert "crm" not in text.lower(), (
            f"{path} hardcodes the CRM plugin instead of using the sockets")


def test_dashboard_lists_only_plugins_the_account_can_use():
    runtime = Runtime(installed=False)
    anonymous = call(runtime.use("GET", "nobody"), "/api/plugins")
    assert anonymous["status"] == 401

    before = call(runtime.use("GET"), "/api/plugins")["data"]["plugins"]
    assert {"tasks", "calendar", "notes", "analytics"} <= set(before)
    assert not {"crm", "calories", "vault", "browser-extension"} & set(before)
    outsider = call(runtime.use("GET", "bob"), "/api/plugins")["data"]["plugins"]
    assert {"tasks", "calendar", "notes"} <= set(outsider)
    assert "analytics" not in outsider

    call(runtime.use("POST"), "/api/plugins/acme/crm/install")
    assert "crm" in call(runtime.use("GET"), "/api/plugins")["data"]["plugins"]
    assert "crm" in call(
        runtime.use("GET", "charlie"), "/api/plugins")["data"]["plugins"]
    assert "crm" not in call(
        runtime.use("GET", "bob"), "/api/plugins")["data"]["plugins"]

    denied = call(runtime.use("POST"), "/api/plugins")
    assert denied["status"] == 405


def test_dashboard_lists_crm_for_an_outsider_a_sheet_is_shared_with():
    runtime = Runtime(installed=False)
    call(runtime.use("POST"), "/api/plugins/acme/crm/install")
    created = crm_call(runtime.use("POST", body={"name": "Partners"}),
                       "/api/crm/acme/views")
    view_id = created["data"]["view"]["id"]
    assert "crm" not in call(
        runtime.use("GET", "bob"), "/api/plugins")["data"]["plugins"]

    crm_call(runtime.use("PATCH", body={"sharedWith": ["bob"]}),
             f"/api/crm/acme/views/{view_id}")
    assert "crm" in call(
        runtime.use("GET", "bob"), "/api/plugins")["data"]["plugins"]

    crm_call(runtime.use("PATCH", body={"sharedWith": []}),
             f"/api/crm/acme/views/{view_id}")
    assert "crm" not in call(
        runtime.use("GET", "bob"), "/api/plugins")["data"]["plugins"]

    crm_call(runtime.use("PATCH", body={"sharedWith": ["bob"]}),
             f"/api/crm/acme/views/{view_id}")
    call(runtime.use("POST"), "/api/plugins/acme/crm/uninstall")
    assert "crm" not in call(
        runtime.use("GET", "bob"), "/api/plugins")["data"]["plugins"]

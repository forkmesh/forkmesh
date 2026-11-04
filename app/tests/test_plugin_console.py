import asyncio
import json
from pathlib import Path
import sqlite3
import sys
import types

import pytest


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
for folder in (ROOT / "src", ROOT / "tools", REPO / "marketplace" / "tools"):
    if str(folder) not in sys.path:
        sys.path.insert(0, str(folder))

import build_app_plugins  # noqa: E402
import plugin_console  # noqa: E402
import plugin_data  # noqa: E402


def _registry():
    namespace = {}
    exec(build_app_plugins.registry_source(), namespace)
    return namespace["PLUGINS"]


REGISTRY = _registry()
NOW = 1_790_000_000_000


class Runtime:
    def __init__(self, admin=True, name="alice"):
        plugin_data._ACCOUNT_SCHEMA_READY = False
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
            CREATE TABLE org_plugin_installs (org_bi TEXT, plugin TEXT, org_name TEXT,
                installed_by_bi TEXT, installed_by_name TEXT, installed_at INTEGER,
                PRIMARY KEY (org_bi, plugin));
            CREATE TABLE org_members (org_bi TEXT, member_bi TEXT, role TEXT);
            CREATE TABLE error_log (id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER NOT NULL,
                status INTEGER NOT NULL, method TEXT, path TEXT, message TEXT, ray TEXT,
                actor TEXT NOT NULL DEFAULT '');
            CREATE TABLE sensitive_audit_log (id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts INTEGER NOT NULL, actor_bi TEXT, actor_label TEXT NOT NULL DEFAULT '',
                action TEXT NOT NULL, target_type TEXT NOT NULL DEFAULT '',
                target_token TEXT NOT NULL DEFAULT '', outcome TEXT NOT NULL,
                details TEXT NOT NULL DEFAULT '{}');
        """)
        self.admin = admin
        self.name = name
        self.method_value = "GET"
        self.query = {}
        self.audits = []

    def method(self): return self.method_value
    def same_origin(self): return True
    def now(self): return NOW
    def query_params(self): return dict(self.query)
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload, "cache": cache_control}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return {}, ""
    async def session(self, _data=None):
        return ("bi:" + self.name, {"name": self.name}) if self.name else ("", None)
    async def is_admin(self, name): return self.admin and name == self.name
    async def organization(self, _name, _account_bi): return "org:acme", "owner"
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor
    async def audit(self, actor, action, target_type="", target="", outcome="success",
                    details=None):
        self.audits.append((actor, action, target_type, target, details or {}))
        self.db.execute(
            "INSERT INTO sensitive_audit_log (ts, actor_label, action, target_type, outcome, details)"
            " VALUES (?,?,?,?,?,?)",
            (NOW, actor, action, target_type, outcome,
             json.dumps(details or {}, sort_keys=True, separators=(",", ":"))))


WATER = types.SimpleNamespace(
    __file__=str(REPO / "marketplace" / "packages" / "water" / "content" / "worker" / "water.py"),
    DATA_SCHEMA_UP=["CREATE TABLE IF NOT EXISTS water_entries (account_bi TEXT, day TEXT, ml INTEGER)"],
)


@pytest.fixture(autouse=True)
def registry(monkeypatch):
    monkeypatch.setattr(plugin_data, "REGISTRY", REGISTRY)
    monkeypatch.setattr(plugin_data, "_module", lambda plugin: WATER if plugin == "water" else None)
    monkeypatch.setattr(plugin_console, "_MODULE_TABLES", {})


def call(runtime, path):
    return asyncio.run(plugin_data.handle(runtime, path))


def test_console_requires_a_signed_in_administrator():
    assert call(Runtime(name=""), "/api/plugins/@console")["status"] == 401
    refused = call(Runtime(admin=False), "/api/plugins/@console")
    assert refused["status"] == 403
    assert refused["data"] == {"error": "admin_required"}
    for path in ("/api/plugins/@console/water", "/api/plugins/@console/water/rows",
                 "/api/plugins/@console/water/export"):
        assert call(Runtime(admin=False), path)["status"] == 403


def test_console_is_read_only_and_never_cached():
    runtime = Runtime()
    runtime.method_value = "POST"
    assert call(runtime, "/api/plugins/@console")["status"] == 405
    result = call(Runtime(), "/api/plugins/@console")
    assert result["cache"].startswith("no-store")
    assert call(Runtime(), "/api/plugins/@console/water/delete")["status"] == 404


def test_list_covers_every_catalog_package_with_its_surfaces():
    runtime = Runtime()
    runtime.db.execute(
        "INSERT INTO org_plugin_installs VALUES ('org:acme','crm','acme','bi:bob','bob',5)")
    data = call(runtime, "/api/plugins/@console")["data"]
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text())
    assert data["count"] == len(catalog["packages"])
    rows = {row["id"]: row for row in data["packages"]}
    crm = rows["crm"]
    assert crm["kind"] == "plugin"
    assert crm["surfaces"]["dashboard"] == "/dashboard/crm"
    assert crm["surfaces"]["worker"] == "crm"
    assert crm["surfaces"]["desktop"] == "module"
    assert crm["installs"]["organizations"] == 1
    assert rows["ledger"]["surfaces"]["desktop"] == "core"
    assert rows["issues"]["kind"] == "switch"
    assert rows["actions"]["surfaces"]["desktop"] == ""
    assert rows["water"]["mine"] == {"installed": False,
                                     "places": rows["water"]["places"]}
    assert all(row["inBuild"] is True for row in data["packages"]
               if row["id"] in REGISTRY)


def test_detail_lists_tables_storage_installs_and_logs():
    runtime = Runtime()
    runtime.db.execute("CREATE TABLE water_entries (account_bi TEXT, day TEXT, ml INTEGER)")
    runtime.db.executemany("INSERT INTO water_entries VALUES (?,?,?)",
                           [("bi:alice", "2026-11-01", 250), ("bi:bob", "2026-11-02", 500)])
    runtime.db.execute(
        "INSERT INTO error_log (ts,status,method,path,message) VALUES (?,?,?,?,?)",
        (NOW - 1000, 500, "GET", "/api/water/days", "boom"))
    runtime.db.execute(
        "INSERT INTO error_log (ts,status,method,path,message) VALUES (?,?,?,?,?)",
        (NOW - 1000, 500, "GET", "/api/waterfall", "other package"))
    call(runtime, "/api/plugins/@account/water/install")
    runtime.method_value = "POST"
    call(runtime, "/api/plugins/@account/water/install")
    runtime.method_value = "GET"
    data = call(runtime, "/api/plugins/@console/water")["data"]
    tables = {table["name"]: table for table in data["tables"]}
    assert tables["water_entries"]["owner"] == "package"
    assert tables["water_entries"]["rows"] == 2
    assert tables["water_entries"]["bytes"] > 0
    assert tables["water_keys"]["exists"] is False
    assert tables["account_package_installs"]["owner"] == "core"
    assert tables["account_package_installs"]["rows"] == 1
    assert data["storage"]["rows"] == 3
    assert data["installs"]["places"]["web"] == 1
    assert [row["path"] for row in data["logs"]["errors"]] == ["/api/water/days"]
    assert [row["action"] for row in data["logs"]["activity"]] == ["plugin.install"]
    assert data["package"]["mine"]["installed"] is True
    assert call(runtime, "/api/plugins/@console/no-such-package")["status"] == 404


def test_row_browser_reads_only_the_package_tables_and_audits():
    runtime = Runtime()
    runtime.db.execute("CREATE TABLE water_entries (account_bi TEXT, day TEXT, ml INTEGER)")
    runtime.db.execute("INSERT INTO water_entries VALUES ('bi:alice','2026-11-01',?)", ("x" * 900,))
    runtime.query = {"table": "water_entries", "limit": "500"}
    data = call(runtime, "/api/plugins/@console/water/rows")["data"]
    assert data["columns"] == ["account_bi", "day", "ml"]
    assert data["limit"] == plugin_console.ROWS_LIMIT
    assert data["total"] == 1
    assert len(data["rows"][0][2]) == plugin_console.VALUE_LIMIT + 1
    assert runtime.audits[-1][1] == "plugin.console.rows"
    for table in ("users", "org_members", "crm_contacts", "water_entries; DROP TABLE x"):
        runtime.query = {"table": table}
        assert call(runtime, "/api/plugins/@console/water/rows")["status"] == 404
    runtime.query = {"table": "water_keys"}
    assert call(runtime, "/api/plugins/@console/water/rows")["status"] == 409


def test_core_rows_are_filtered_to_the_package():
    runtime = Runtime()
    plugin_data._ACCOUNT_SCHEMA_READY = False
    asyncio.run(plugin_data._ensure_account_schema(runtime))
    runtime.db.executemany(
        "INSERT INTO account_package_installs (account_bi,package,installed) VALUES (?,?,1)",
        [("bi:a", "water"), ("bi:b", "sleep")])
    runtime.query = {"table": "account_package_installs"}
    data = call(runtime, "/api/plugins/@console/water/rows")["data"]
    package = data["columns"].index("package")
    assert [row[package] for row in data["rows"]] == ["water"]


def test_export_dumps_the_package_tables_and_audits():
    runtime = Runtime()
    runtime.db.execute("CREATE TABLE water_entries (account_bi TEXT, day TEXT, ml INTEGER)")
    runtime.db.execute("INSERT INTO water_entries VALUES ('bi:alice','2026-11-01',250)")
    data = call(runtime, "/api/plugins/@console/water/export")["data"]
    assert data["filename"] == "forkmesh-water-tables.json"
    assert data["tables"]["water_entries"]["rows"] == [["bi:alice", "2026-11-01", 250]]
    assert data["truncated"] is False
    assert runtime.audits[-1][1] == "plugin.console.export"
    assert runtime.audits[-1][4]["package"] == "water"


def test_package_tables_never_include_core_schema_tables():
    tables = plugin_console._module_tables("water")
    assert "water_entries" in tables
    core = plugin_console._core_schema_tables()
    assert "users" in core
    assert not set(table.lower() for table in tables) & core


def test_lifecycle_audits_name_the_package():
    source = (ROOT / "src" / "plugin_data.py").read_text(encoding="utf-8")
    assert source.count('details={"package": plugin}') == 6


def test_console_page_is_a_composed_admin_dashboard_page():
    import tomllib
    import dashboard_bundle
    import dashboard_shell
    import static_routes

    page = dashboard_shell.PAGES["plugins"]
    assert page["route"] == "/dashboard/plugins"
    assert static_routes.DASHBOARD_PAGE_ASSETS["/dashboard/plugins"] == page["asset"]
    assert "/dashboard/plugins/index.html" in static_routes.BLOCKED_STATIC_HTML_PATHS
    first = tomllib.loads((ROOT / "wrangler.toml").read_text())["assets"]["run_worker_first"]
    assert "!/dashboard/plugins" in first and "!/dashboard/plugins/" in first
    assert dashboard_bundle.LAZY_CHUNKS["plugins"] == "09-plugins.js"
    assert dashboard_shell.CHUNK_PAGES["plugins"] == "plugins"
    assert "dashboard-page-plugins.js" in dashboard_shell.CACHE_BUSTED_ASSETS
    boot = (ROOT / "public" / "dashboard" / "js" / "10-boot.js").read_text()
    assert '"plugins": initPluginsPage,' in boot
    built = (ROOT / "public" / page["asset"]).read_text()
    assert built.count("<section data-view=") == 1
    assert 'data-page="plugins"' in built
    assert "/dashboard-page-plugins.js" in built
    sidebar = (ROOT / "public" / "dashboard" / "partials" / "sidebar.html").read_text()
    link = next(line for line in sidebar.splitlines() if 'data-nav="plugins"' in line)
    assert "data-admin-only" in link and 'class="hidden"' in link


def test_console_script_uses_the_console_and_existing_install_apis():
    script = (ROOT / "public" / "dashboard" / "js" / "09-plugins.js").read_text()
    for path in ("/api/plugins/@console", "/api/plugins/@account/", "/places", "/rows?",
                 "/export"):
        assert path in script, path
    assert "fm-btn-cta" not in script
    assert "fm-btn-danger" in script
    view = (ROOT / "public" / "dashboard" / "partials" / "views" / "plugins.html").read_text()
    assert "--dashboard-primary-rgb" in view
    assert "cta" not in view and "success-rgb" not in view

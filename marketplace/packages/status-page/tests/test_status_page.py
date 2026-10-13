import ast
import asyncio
import json
from pathlib import Path
import sqlite3
import sys

import pytest


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
MARKET = REPO / "marketplace"
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
sys.path.insert(0, str(REPO / "app" / "src"))
sys.path.insert(0, str(REPO / "app" / "tools"))
sys.path.insert(0, str(MARKET / "tools"))
import status_monitors  # noqa: E402
import plugin_data  # noqa: E402
import marketplace_lib  # noqa: E402
import build_app_plugins  # noqa: E402
import market_packages  # noqa: E402


ACCOUNTS = {"alice", "bob"}
CHECKS = sorted(path.name for path in (MARKET / "packages").iterdir()
                if path.name.startswith("status-check-"))


def _registry():
    namespace = {}
    exec(build_app_plugins.registry_source(), namespace)
    return namespace["PLUGINS"]


REGISTRY = _registry()


class Runtime:
    def __init__(self):
        status_monitors._SCHEMA_READY = False
        plugin_data._ACCOUNT_SCHEMA_READY = False
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.execute("CREATE TABLE org_plugin_installs (org_bi TEXT, plugin TEXT)")
        self.db.execute("CREATE TABLE org_members (org_bi TEXT, member_bi TEXT, role TEXT)")
        self.actor = "alice"
        self.method_value = "GET"
        self.body = {}
        self.clock = 1_789_561_860_000
        self.ids = 0
        self.audits = []
        self._plugin = "status-page"

    def use(self, method, actor="alice", body=None):
        self.method_value, self.actor, self.body = method, actor, body or {}
        return self

    def method(self): return self.method_value
    def same_origin(self): return True
    def now(self): self.clock += 1; return self.clock
    def new_id(self): self.ids += 1; return f"{self.ids:032x}"
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload, "headers": extra_headers or {}}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data=None):
        bi = "bi:" + self.actor if self.actor in ACCOUNTS else ""
        return (bi, {"name": self.actor}) if bi else ("", None)
    async def seal(self, value): return "sealed:" + json.dumps(value, sort_keys=True)
    async def open(self, value):
        text = str(value or "")
        return json.loads(text.removeprefix("sealed:")) if text.startswith("sealed:") else None
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor
    async def audit(self, *args, **kwargs): self.audits.append(args)
    async def account_plugin_installed(self, account_bi):
        return await plugin_data.account_package_installed(self, account_bi, self._plugin)


@pytest.fixture(autouse=True)
def registry(monkeypatch):
    monkeypatch.setattr(plugin_data, "REGISTRY", REGISTRY)
    monkeypatch.setattr(plugin_data, "_module", lambda plugin: (
        status_monitors if plugin == "status-page" else None))


def lifecycle(runtime, path):
    return asyncio.run(plugin_data.handle(runtime, path))


def monitors(runtime, path="/api/status-monitors"):
    return asyncio.run(status_monitors.handle(runtime, path))


def state(payload):
    return {row["id"]: row for row in payload["packages"]}


def test_every_check_is_an_account_synced_dependent_of_status_page():
    assert len(CHECKS) == 18
    parent = json.loads((PACKAGE / "manifest.json").read_text())
    assert parent["install"] == {
        "bundled": True, "default": False, "sync": "account", "installedBefore": "2026-10-11"}
    assert [row["key"] for row in parent["settings"]] == ["onlyProblems", "refreshMinutes"]
    for check in CHECKS:
        root = MARKET / "packages" / check
        manifest = json.loads((root / "manifest.json").read_text())
        assert manifest["dependencies"] == [{"id": "status-page", "version": "^2.0.0"}]
        assert manifest["install"]["sync"] == "account"
        [contribution] = manifest["contributions"]
        assert contribution["socket"] == "status.monitor"
        monitor = json.loads((root / "content" / "monitor.json").read_text())
        assert contribution["config"] == monitor
        assert (root / "preview" / "market.webp").is_file()


def test_checks_cover_every_status_row():
    entry = REPO / "app" / "src" / "entry.py"
    tree = ast.parse(entry.read_text(encoding="utf-8"))
    systems = next(
        ast.literal_eval(node.value) for node in tree.body
        if isinstance(node, ast.Assign)
        and any(getattr(target, "id", "") == "STATUS_SYSTEMS" for target in node.targets))
    rows = {row["id"]: row for row in market_packages.catalog_packages()}
    covered = set()
    prefixes = set()
    for check in CHECKS:
        covered.update(rows[check]["monitors"]["systems"])
        prefixes.update(rows[check]["monitors"]["prefixes"])
        assert rows[check]["dependencies"] == ["status-page"]
        assert rows[check]["accountSync"] is True
        assert rows[check]["default"] is (check != "status-check-mirrors")
    assert {system for system, _label in systems} <= covered
    assert {"desktop_website", "desktop_status_page"} <= covered
    assert prefixes == {"core:", "runner:", "mirror:"}
    page = rows["status-page"]
    assert page["settings"][1]["options"][0] == {"value": "2", "label": "2 minutes"}
    assert page["surfaces"]["web"] == "/dashboard/status-monitors"


def test_registry_carries_sync_dependencies_and_settings():
    assert REGISTRY["status-page"]["accountSync"] is True
    assert REGISTRY["status-page"]["accountData"] is True
    assert REGISTRY["status-check-api"]["dependencies"] == ("status-page",)
    assert REGISTRY["status-page"]["settings"][0]["default"] is False


def status_rows(payload):
    return {plugin: row for plugin, row in state(payload).items()
            if plugin == "status-page" or plugin in CHECKS}


def test_fresh_account_has_every_default_installed():
    runtime = Runtime()
    payload = lifecycle(runtime.use("GET"), "/api/plugins/@account")["data"]
    rows = status_rows(payload)
    assert set(rows) == {"status-page", *CHECKS}
    assert [plugin for plugin, row in rows.items() if not row["installed"]] == [
        "status-check-mirrors"]
    assert rows["status-page"]["settings"] == {"onlyProblems": False, "refreshMinutes": "15"}
    visible = lifecycle(runtime.use("GET"), "/api/plugins")["data"]["plugins"]
    assert "status-page" in visible and "status-check-cores" in visible


def test_uninstalling_status_page_removes_every_check_and_monitor_everywhere():
    runtime = Runtime()
    created = monitors(runtime.use("POST", body={"name": "Blog", "url": "https://blog.example.com/"}),
                       "/api/status-monitors/monitors")
    assert created["status"] == 200
    lifecycle(runtime.use("POST", body={"settings": {"onlyProblems": True}}),
              "/api/plugins/@account/status-page/settings")
    lifecycle(runtime.use("POST"), "/api/plugins/@account/status-check-api/uninstall")
    payload = lifecycle(runtime.use("POST"), "/api/plugins/@account/status-page/uninstall")["data"]
    rows = status_rows(payload)
    assert not any(row["installed"] for row in rows.values())
    assert set(payload["changed"]) == {"status-page", *CHECKS} - {
        "status-check-api", "status-check-mirrors"}
    assert rows["status-page"]["settings"]["onlyProblems"] is False
    assert runtime.db.execute("SELECT COUNT(*) FROM status_monitor_lists").fetchone()[0] == 0
    assert "status-page" not in lifecycle(runtime.use("GET"), "/api/plugins")["data"]["plugins"]
    listing = monitors(runtime.use("GET"))["data"]
    assert listing == {"ok": True, "installed": False, "max": 20, "monitors": []}
    assert monitors(runtime.use("POST", body={"name": "x", "url": "https://x.example"}),
                    "/api/status-monitors/monitors")["status"] == 409
    assert state(lifecycle(runtime.use("GET", actor="bob"), "/api/plugins/@account")["data"])[
        "status-page"]["installed"] is True

    payload = lifecycle(runtime.use("POST"), "/api/plugins/@account/status-page/install")["data"]
    rows = state(payload)
    assert not rows["status-check-mirrors"]["installed"]
    assert all(rows[check]["installed"] for check in CHECKS if check != "status-check-mirrors")
    assert set(payload["changed"]) == {"status-page", *CHECKS} - {"status-check-mirrors"}


def test_installing_a_check_brings_status_page_back():
    runtime = Runtime()
    lifecycle(runtime.use("POST"), "/api/plugins/@account/status-page/uninstall")
    payload = lifecycle(runtime.use("POST"), "/api/plugins/@account/status-check-cores/install")["data"]
    rows = state(payload)
    assert rows["status-page"]["installed"] and rows["status-check-cores"]["installed"]
    assert set(payload["changed"]) == set(CHECKS) - {"status-check-mirrors"} | {"status-page"}


def test_removing_one_check_keeps_the_rest():
    runtime = Runtime()
    payload = lifecycle(runtime.use("POST"), "/api/plugins/@account/status-check-email/uninstall")["data"]
    rows = state(payload)
    assert payload["changed"] == ["status-check-email"]
    assert rows["status-page"]["installed"] and not rows["status-check-email"]["installed"]
    assert "status-check-email" not in lifecycle(runtime.use("GET"), "/api/plugins")["data"]["plugins"]


def test_settings_are_validated_against_the_manifest():
    runtime = Runtime()
    good = lifecycle(runtime.use("POST", body={"settings": {"refreshMinutes": "5"}}),
                     "/api/plugins/@account/status-page/settings")
    assert state(good["data"])["status-page"]["settings"]["refreshMinutes"] == "5"
    for body in ({"settings": {"refreshMinutes": "7"}}, {"settings": {"onlyProblems": "yes"}},
                 {"settings": {"nope": True}}, {"settings": []}):
        bad = lifecycle(runtime.use("POST", body=body), "/api/plugins/@account/status-page/settings")
        assert bad["status"] == 400 and bad["data"]["error"] == "invalid_setting"


def test_account_routes_need_a_session_and_a_synced_package():
    runtime = Runtime()
    assert lifecycle(runtime.use("GET", actor="mallory"), "/api/plugins/@account")["status"] == 401
    assert lifecycle(runtime.use("POST"), "/api/plugins/@account/missing/install")["status"] == 404
    assert lifecycle(runtime.use("POST"), "/api/plugins/@account/sign-in/install")["data"] == {
        "error": "plugin_not_account_synced"}
    assert lifecycle(runtime.use("GET"), "/api/plugins/@account/status-page/install")["status"] == 405


def test_custom_monitors_crud_and_validation():
    runtime = Runtime()
    first = monitors(runtime.use("POST", body={"name": "  Blog  ", "url": "https://blog.example.com/"}),
                     "/api/status-monitors/monitors")["data"]
    monitor_id = first["monitor"]["id"]
    assert first["monitors"] == [{"id": monitor_id, "name": "Blog", "url": "https://blog.example.com/"}]
    stored = runtime.db.execute("SELECT data FROM status_monitor_lists").fetchone()[0]
    assert stored.startswith("sealed:")
    updated = monitors(runtime.use("PUT", body={"name": "Shop", "url": "http://shop.example:8080/health"}),
                       "/api/status-monitors/monitors/" + monitor_id)["data"]
    assert updated["monitor"]["name"] == "Shop"
    assert monitors(runtime.use("GET", actor="bob"))["data"]["monitors"] == []
    for body, error in (({"name": "", "url": "https://a.example"}, "invalid_name"),
                        ({"name": "x" * 41, "url": "https://a.example"}, "invalid_name"),
                        ({"name": "A", "url": "ftp://a.example"}, "invalid_url"),
                        ({"name": "A", "url": "https://user:pw@a.example"}, "invalid_url"),
                        ({"name": "A", "url": "https://a.example/ x"}, "invalid_url"),
                        ({"name": "A", "url": "javascript:alert(1)"}, "invalid_url"),
                        ({"name": "A", "url": "https://" + "a" * 300}, "invalid_url")):
        response = monitors(runtime.use("POST", body=body), "/api/status-monitors/monitors")
        assert response["status"] == 400 and response["data"]["error"] == error
    deleted = monitors(runtime.use("DELETE"), "/api/status-monitors/monitors/" + monitor_id)["data"]
    assert deleted["monitors"] == []
    assert monitors(runtime.use("DELETE"), "/api/status-monitors/monitors/" + monitor_id)["status"] == 404


def test_custom_monitors_are_capped():
    runtime = Runtime()
    for index in range(status_monitors.MAX_MONITORS):
        assert monitors(runtime.use("POST", body={"name": f"M{index}", "url": f"https://m{index}.example"}),
                        "/api/status-monitors/monitors")["status"] == 200
    over = monitors(runtime.use("POST", body={"name": "One more", "url": "https://x.example"}),
                    "/api/status-monitors/monitors")
    assert over["status"] == 409 and over["data"]["error"] == "too_many_monitors"


def test_account_deletion_purges_install_state_and_monitors():
    runtime = Runtime()
    monitors(runtime.use("POST", body={"name": "Blog", "url": "https://blog.example.com/"}),
             "/api/status-monitors/monitors")
    lifecycle(runtime.use("POST"), "/api/plugins/@account/status-check-api/uninstall")
    asyncio.run(plugin_data.purge_account(runtime, "bi:alice", "alice"))
    assert runtime.db.execute("SELECT COUNT(*) FROM account_package_installs").fetchone()[0] == 0
    assert runtime.db.execute("SELECT COUNT(*) FROM status_monitor_lists").fetchone()[0] == 0


def test_catalog_rejects_unknown_dependencies_and_cycles():
    manifests = {
        "a": {"dependencies": [{"id": "b", "version": "1"}], "install": {"sync": "account"}},
        "b": {"dependencies": [{"id": "a", "version": "1"}], "install": {"sync": "account"}},
    }
    with pytest.raises(marketplace_lib.MarketplaceError, match="cycle"):
        marketplace_lib.validate_dependencies(manifests)
    with pytest.raises(marketplace_lib.MarketplaceError, match="not in the catalog"):
        marketplace_lib.validate_dependencies({"a": {"dependencies": [{"id": "z", "version": "1"}]}})
    with pytest.raises(marketplace_lib.MarketplaceError, match="install.sync"):
        marketplace_lib.validate_dependencies({
            "a": {"dependencies": [{"id": "b", "version": "1"}], "install": {"sync": "account"}},
            "b": {"dependencies": []},
        })


@pytest.mark.parametrize("setting", [
    {"key": "x", "type": "boolean", "label": "X", "default": "no"},
    {"key": "x", "type": "choice", "label": "X", "default": "c",
     "options": [{"value": "a", "label": "A"}, {"value": "b", "label": "B"}]},
    {"key": "x", "type": "choice", "label": "X", "default": "a", "options": [{"value": "a", "label": "A"}]},
    {"key": "X", "type": "boolean", "label": "X", "default": True},
    {"key": "x", "type": "number", "label": "X", "default": 1},
])
def test_invalid_settings_are_rejected(setting):
    with pytest.raises(marketplace_lib.MarketplaceError):
        marketplace_lib.validate_settings([setting], "demo")

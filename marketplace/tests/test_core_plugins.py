import asyncio
from datetime import datetime, timezone
import json
from pathlib import Path
import sqlite3
import sys
import types

import pytest


REPO = Path(__file__).resolve().parents[2]
MARKET = REPO / "marketplace"
sys.path.insert(0, str(REPO / "app" / "src"))
sys.path.insert(0, str(REPO / "app" / "tools"))
sys.path.insert(0, str(MARKET / "tools"))
import build_app_plugins  # noqa: E402
import market_packages  # noqa: E402
import plugin_data  # noqa: E402


CUTOFF = "2026-10-11"
NEW_SYNCED = (
    "actions", "activity", "agents", "branches", "bug-reports", "chat", "code", "commits",
    "coves", "desktops", "discussions", "hosts", "inbox", "issues", "logs", "network", "on-device-ai",
    "organizations", "pull-requests", "releases", "shortcuts", "special-agents", "stats",
    "world")
MOBILE_SYNCED = ("diagnostics",)
CONVERTED_LEGACY = (
    "calendar", "design-system", "glasses-alert", "ledger", "notes", "speech", "status-page",
    "tasks", "tour")
CONVERTED_OFF = ("calories", "sleep", "steps", "stretch", "sugar-break", "timers", "vault", "water")
HOOKED = ("calories", "sleep", "steps", "stretch", "sugar-break", "vault", "water")
CORE = ("market", "repos", "search", "settings")
WORKFLOWS_CUTOFF = "2026-10-14"


def _ms(day):
    moment = datetime.strptime(day, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    return int(moment.timestamp() * 1000)


OLD = _ms("2025-03-01")
NEW = _ms("2026-10-20")


def _registry():
    namespace = {}
    exec(build_app_plugins.registry_source(), namespace)
    return namespace["PLUGINS"]


REGISTRY = _registry()


class Hooked:
    def __init__(self):
        self.installed = set()
        self.purged = []
        self.calls = []

    def module(self, plugin):
        async def account_installed(runtime, account_bi):
            self.calls.append(plugin)
            return (plugin, account_bi) in self.installed

        async def purge_account_data(runtime, account_bi, account_name=""):
            self.purged.append(plugin)
            self.installed.discard((plugin, account_bi))

        return types.SimpleNamespace(
            account_installed=account_installed, purge_account_data=purge_account_data)


class Runtime:
    def __init__(self, created_at=None):
        plugin_data._ACCOUNT_SCHEMA_READY = False
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.execute("CREATE TABLE org_plugin_installs (org_bi TEXT, plugin TEXT)")
        self.db.execute("CREATE TABLE org_members (org_bi TEXT, member_bi TEXT, role TEXT)")
        self.created_at = created_at
        self.method_value = "GET"
        self.clock = 1_789_561_860_000
        self.lookups = 0

    def use(self, method):
        self.method_value = method
        return self

    def method(self): return self.method_value
    def same_origin(self): return True
    def now(self): self.clock += 1; return self.clock
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return {}, ""
    async def session(self, _data=None):
        record = {"name": "alice"}
        if self.created_at is not None:
            record["created_at"] = self.created_at
        return "bi:alice", record
    async def account_created_at(self, _account_bi):
        self.lookups += 1
        return int(self.created_at or 0)
    async def organization(self, _name, _account_bi): return "org:acme", "owner"
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor
    async def audit(self, *args, **kwargs): pass


@pytest.fixture
def hooks(monkeypatch):
    hooked = Hooked()
    monkeypatch.setattr(plugin_data, "REGISTRY", REGISTRY)
    monkeypatch.setattr(plugin_data, "_module", lambda plugin: (
        hooked.module(plugin) if plugin in HOOKED else None))
    return hooked


def call(runtime, path, method="GET"):
    return asyncio.run(plugin_data.handle(runtime.use(method), path))


def installed(runtime):
    payload = call(runtime, "/api/plugins/@account")["data"]
    return {row["id"]: row["installed"] for row in payload["packages"]}


def test_every_gated_package_is_account_synced_off_by_default_and_dated():
    for plugin in NEW_SYNCED + MOBILE_SYNCED + CONVERTED_LEGACY:
        entry = REGISTRY[plugin]
        assert entry["accountSync"] and not entry["default"], plugin
        assert entry["installedBefore"] == CUTOFF, plugin
    for plugin in CONVERTED_OFF:
        entry = REGISTRY[plugin]
        assert entry["accountSync"] and not entry["default"], plugin
        assert entry["installedBefore"] == "", plugin
    assert REGISTRY["sign-in"]["installedBefore"] == CUTOFF
    assert not REGISTRY["sign-in"]["accountSync"] and not REGISTRY["sign-in"]["default"]
    assert REGISTRY["pull-requests"]["dependencies"] == ("branches",)
    assert REGISTRY["special-agents"]["dependencies"] == ("agents",)
    assert not set(CORE) & set(REGISTRY)
    assert not REGISTRY["workflows"]["default"]
    assert REGISTRY["workflows"]["installedBefore"] == WORKFLOWS_CUTOFF
    assert REGISTRY["crm"]["organizationData"] and not REGISTRY["crm"]["accountSync"]


def test_market_rows_carry_installed_before_only_when_set():
    rows = {row["id"]: row for row in market_packages.catalog_packages()}
    for plugin in NEW_SYNCED + MOBILE_SYNCED + CONVERTED_LEGACY + ("sign-in",):
        assert rows[plugin]["installedBefore"] == CUTOFF, plugin
        assert rows[plugin]["default"] is False and rows[plugin]["accountSync"] is (
            plugin != "sign-in")
    for plugin in CONVERTED_OFF + ("crm",):
        assert "installedBefore" not in rows[plugin], plugin
    assert rows["workflows"]["installedBefore"] == WORKFLOWS_CUTOFF
    assert rows["pull-requests"]["dependencies"] == ["branches"]
    assert rows["special-agents"]["dependencies"] == ["agents"]
    assert market_packages.installed_before({"install": {"installedBefore": "2026-13-01"}}) == ""


def test_a_new_account_starts_with_every_gated_package_removed(hooks):
    runtime = Runtime(NEW)
    state = installed(runtime)
    for plugin in NEW_SYNCED + MOBILE_SYNCED + CONVERTED_LEGACY + CONVERTED_OFF:
        assert state[plugin] is False, plugin
    assert not any(state[plugin] for plugin in state if plugin.startswith("status-check-"))
    assert state["workflows"] is False and state["workflows-examples"] is False
    visible = call(runtime, "/api/plugins")["data"]["plugins"]
    assert not set(NEW_SYNCED + CONVERTED_LEGACY + CONVERTED_OFF + ("sign-in",)) & set(visible)
    assert "workflows" not in visible
    assert runtime.lookups == 0


def test_an_account_created_on_the_cutoff_day_is_new(hooks):
    state = installed(Runtime(_ms(CUTOFF)))
    assert not any(state[plugin] for plugin in NEW_SYNCED)


def test_an_older_account_keeps_every_dated_package(hooks):
    runtime = Runtime(OLD)
    state = installed(runtime)
    for plugin in NEW_SYNCED + MOBILE_SYNCED + CONVERTED_LEGACY:
        assert state[plugin] is True, plugin
    for plugin in CONVERTED_OFF:
        assert state[plugin] is False, plugin
    assert state["status-check-cores"] is True and state["status-check-mirrors"] is False
    visible = call(runtime, "/api/plugins")["data"]["plugins"]
    assert set(NEW_SYNCED + CONVERTED_LEGACY + ("sign-in",)) <= set(visible)


def test_only_dated_fallback_installs_are_marked_legacy(hooks):
    runtime = Runtime(OLD)
    call(runtime, "/api/plugins/@account/notes/uninstall", "POST")
    call(runtime, "/api/plugins/@account/agents/install", "POST")
    payload = call(runtime, "/api/plugins/@account")["data"]
    legacy = {row["id"] for row in payload["packages"] if row.get("legacy")}
    assert set(NEW_SYNCED + MOBILE_SYNCED + CONVERTED_LEGACY) - {"notes", "agents"} <= legacy
    assert "workflows" in legacy
    assert not legacy & ({"notes", "agents"} | set(CONVERTED_OFF))
    fresh = call(Runtime(NEW), "/api/plugins/@account")["data"]
    assert not any(row.get("legacy") for row in fresh["packages"])


@pytest.mark.parametrize("created_at", [None, 0, "garbage"])
def test_a_missing_creation_time_counts_as_an_old_account(hooks, created_at):
    state = installed(Runtime(created_at))
    assert all(state[plugin] for plugin in NEW_SYNCED + MOBILE_SYNCED + CONVERTED_LEGACY)


def test_installing_pull_requests_brings_branches(hooks):
    runtime = Runtime(NEW)
    payload = call(runtime, "/api/plugins/@account/pull-requests/install", "POST")["data"]
    assert payload["changed"] == ["branches", "pull-requests"]
    state = installed(runtime)
    assert state["pull-requests"] and state["branches"] and not state["issues"]
    payload = call(runtime, "/api/plugins/@account/branches/uninstall", "POST")["data"]
    assert payload["changed"] == ["branches", "pull-requests"]
    state = installed(runtime)
    assert not state["pull-requests"] and not state["branches"]


def test_uninstalling_agents_removes_special_agents(hooks):
    runtime = Runtime(OLD)
    payload = call(runtime, "/api/plugins/@account/agents/uninstall", "POST")["data"]
    assert payload["changed"] == ["agents", "special-agents"]
    state = installed(runtime)
    assert not state["agents"] and not state["special-agents"] and state["inbox"]
    payload = call(runtime, "/api/plugins/@account/special-agents/install", "POST")["data"]
    assert payload["changed"] == ["agents", "special-agents"]


def test_a_new_account_installing_special_agents_gets_agents_too(hooks):
    runtime = Runtime(NEW)
    call(runtime, "/api/plugins/@account/special-agents/install", "POST")
    state = installed(runtime)
    assert state["agents"] and state["special-agents"]


def test_account_package_installed_looks_up_the_creation_time_only_when_needed(hooks):
    runtime = Runtime(NEW)
    assert not asyncio.run(plugin_data.account_package_installed(runtime, "bi:alice", "status-page"))
    assert runtime.lookups == 1
    call(runtime, "/api/plugins/@account/status-page/install", "POST")
    assert asyncio.run(plugin_data.account_package_installed(runtime, "bi:alice", "status-page"))
    assert runtime.lookups == 1
    assert asyncio.run(plugin_data.account_package_installed(runtime, "bi:alice", "timers")) is False
    assert runtime.lookups == 1
    old = Runtime(OLD)
    assert asyncio.run(plugin_data.account_package_installed(old, "bi:alice", "status-page"))
    assert asyncio.run(plugin_data.account_package_installed(
        Runtime(NEW), "bi:alice", "status-page", created_at=OLD))


def test_data_packages_without_a_row_follow_their_own_setup_hook(hooks):
    runtime = Runtime(NEW)
    assert installed(runtime)["vault"] is False
    assert set(HOOKED) <= set(hooks.calls)
    hooks.installed.add(("vault", "bi:alice"))
    assert installed(runtime)["vault"] is True
    assert "vault" in call(runtime, "/api/plugins")["data"]["plugins"]
    assert asyncio.run(plugin_data.account_package_installed(runtime, "bi:alice", "vault"))
    payload = call(runtime, "/api/plugins/@account/vault/uninstall", "POST")["data"]
    assert payload["changed"] == ["vault"] and "vault" in hooks.purged
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM account_package_installs WHERE package='vault'").fetchone()[0] == 0
    assert installed(runtime)["vault"] is False
    hooks.installed.add(("vault", "bi:alice"))
    assert installed(runtime)["vault"] is True


def test_hooks_are_not_called_for_packages_with_a_row(hooks):
    runtime = Runtime(NEW)
    call(runtime, "/api/plugins/@account/calories/install", "POST")
    hooks.calls.clear()
    assert installed(runtime)["calories"] is True
    assert "calories" not in hooks.calls and "vault" in hooks.calls


def test_status_page_never_falls_back_to_its_own_hook(hooks):
    assert plugin_data._hook_fallback("vault")
    assert not plugin_data._hook_fallback("status-page")
    assert not plugin_data._hook_fallback("timers")


def test_settings_on_a_fallback_package_keep_it_installed(hooks):
    runtime = Runtime(OLD)
    runtime.body = None
    payload = asyncio.run(plugin_data._account_save_settings(
        runtime, "bi:alice", "alice", "status-page", {"settings": {"onlyProblems": True}}, OLD))
    rows = {row["id"]: row for row in payload["data"]["packages"]}
    assert rows["status-page"]["installed"] is True
    assert rows["status-page"]["settings"]["onlyProblems"] is True
    stored = runtime.db.execute(
        "SELECT installed,settings FROM account_package_installs WHERE package='status-page'"
    ).fetchone()
    assert stored[0] == 1 and json.loads(stored[1])["onlyProblems"] is True


def test_organizations_cannot_install_account_synced_packages(hooks):
    runtime = Runtime(NEW)
    for plugin in ("vault", "calories", "agents", "pull-requests"):
        result = call(runtime, "/api/plugins/acme/" + plugin + "/install", "POST")
        assert result["status"] == 409 and result["data"]["error"] == "plugin_installs_per_account"


def test_workflows_stay_for_accounts_from_before_the_change_and_bring_their_packs(hooks):
    old = installed(Runtime(_ms("2026-10-13")))
    assert old["workflows"] and old["workflows-examples"] and old["workflows-pack-ai"]
    runtime = Runtime(_ms(WORKFLOWS_CUTOFF))
    assert not installed(runtime)["workflows"]
    payload = call(runtime, "/api/plugins/@account/workflows/install", "POST")["data"]
    assert "workflows" in payload["changed"] and "workflows-examples" in payload["changed"]


def test_data_packages_set_up_without_a_row_are_marked_legacy(hooks):
    runtime = Runtime(NEW)
    hooks.installed.add(("vault", "bi:alice"))
    rows = {row["id"]: row for row in call(runtime, "/api/plugins/@account")["data"]["packages"]}
    assert rows["vault"]["installed"] and rows["vault"]["legacy"]
    call(runtime, "/api/plugins/@account/vault/install", "POST")
    rows = {row["id"]: row for row in call(runtime, "/api/plugins/@account")["data"]["packages"]}
    assert rows["vault"]["installed"] and "legacy" not in rows["vault"]


class Body(Runtime):
    def __init__(self, created_at=None):
        super().__init__(created_at)
        self.body = {}

    def send(self, body):
        self.body = body
        return self

    async def json_body(self, _limit):
        return self.body, ""


def packages(payload):
    return {row["id"]: row for row in payload["packages"]}


def test_every_package_offers_its_declared_places(hooks):
    rows = {row["id"]: row for row in market_packages.catalog_packages()}
    assert rows["food-guide"]["places"] == ["mobile"]
    assert rows["calories"]["places"] == ["mobile", "desktop", "web"]
    assert REGISTRY["food-guide"]["places"] == ("mobile",)
    state = packages(call(Runtime(NEW), "/api/plugins/@account")["data"])
    assert state["food-guide"]["availablePlaces"] == ["mobile"]
    assert state["food-guide"]["places"] == ["mobile"]
    assert state["vault"]["places"] == ["mobile", "desktop", "web"]


def test_an_install_shows_where_the_user_picked(hooks):
    runtime = Body(NEW)
    payload = call(runtime.send({"places": ["web", "mobile"]}), "/api/plugins/@account/vault/install", "POST")
    assert payload["status"] == 200
    assert packages(payload["data"])["vault"]["places"] == ["mobile", "web"]
    assert "vault" in call(runtime, "/api/plugins")["data"]["plugins"]
    payload = call(runtime.send({"places": ["desktop"]}), "/api/plugins/@account/vault/places", "POST")
    vault = packages(payload["data"])["vault"]
    assert vault["installed"] and vault["places"] == ["desktop"]
    assert "vault" not in call(runtime, "/api/plugins")["data"]["plugins"]
    payload = call(runtime.send({"settings": {}}), "/api/plugins/@account/vault/settings", "POST")
    assert packages(payload["data"])["vault"]["places"] == ["desktop"]
    payload = call(runtime.send({}), "/api/plugins/@account/vault/install", "POST")
    assert packages(payload["data"])["vault"]["places"] == ["desktop"]
    call(runtime.send({}), "/api/plugins/@account/vault/uninstall", "POST")
    payload = call(runtime.send({}), "/api/plugins/@account/vault/install", "POST")
    assert packages(payload["data"])["vault"]["places"] == ["mobile", "desktop", "web"]


def test_a_dependency_installed_with_a_child_follows_its_places(hooks):
    runtime = Body(NEW)
    payload = call(runtime.send({"places": ["desktop", "web"]}),
                   "/api/plugins/@account/pull-requests/install", "POST")["data"]
    assert packages(payload)["branches"]["places"] == ["desktop", "web"]
    payload = call(runtime.send({"places": ["mobile"]}),
                   "/api/plugins/@account/pull-requests/install", "POST")["data"]
    assert packages(payload)["pull-requests"]["places"] == ["mobile"]
    assert packages(payload)["branches"]["places"] == ["desktop", "web"]


@pytest.mark.parametrize("places", [[], ["phone"], "web", ["desktop"], [1]])
def test_places_outside_what_the_package_offers_are_refused(hooks, places):
    runtime = Body(NEW)
    plugin = "food-guide" if places == ["desktop"] else "vault"
    result = call(runtime.send({"places": places}), "/api/plugins/@account/" + plugin + "/install", "POST")
    assert result["status"] == 400 and result["data"]["error"] == "invalid_places"


def test_places_need_an_installed_package(hooks):
    runtime = Body(NEW)
    result = call(runtime.send({"places": ["web"]}), "/api/plugins/@account/vault/places", "POST")
    assert result["status"] == 409 and result["data"]["error"] == "plugin_not_installed"
    result = call(runtime.send({}), "/api/plugins/@account/vault/places", "POST")
    assert result["status"] == 400


def test_an_older_install_table_gains_the_places_column(hooks):
    runtime = Runtime(NEW)
    runtime.db.execute("""CREATE TABLE account_package_installs (
        account_bi TEXT NOT NULL, package TEXT NOT NULL,
        installed INTEGER NOT NULL CHECK(installed IN (0, 1)),
        settings TEXT NOT NULL DEFAULT '{}',
        updated_at INTEGER NOT NULL DEFAULT 0 CHECK(updated_at >= 0),
        PRIMARY KEY (account_bi, package))""")
    runtime.db.execute(
        "INSERT INTO account_package_installs VALUES ('bi:alice','calories',1,'{}',1)")
    state = packages(call(runtime, "/api/plugins/@account")["data"])
    assert state["calories"]["installed"] and state["calories"]["places"] == ["mobile", "desktop", "web"]
    plugin_data._ACCOUNT_SCHEMA_READY = False
    assert installed(runtime)["calories"]


class InstanceRuntime(Runtime):
    def __init__(self, created_at=None, name="Acme Code"):
        super().__init__(created_at)
        self.env = types.SimpleNamespace(INSTANCE_NAME=name)


REPO_TABS = ("code", "issues", "pull-requests", "branches", "commits", "releases",
             "discussions", "actions", "agents")


def test_a_new_account_on_a_branded_instance_gets_every_dated_package(hooks):
    runtime = InstanceRuntime(NEW)
    state = installed(runtime)
    for plugin in NEW_SYNCED + MOBILE_SYNCED + CONVERTED_LEGACY:
        assert state[plugin] is True, plugin
    for plugin in CONVERTED_OFF:
        assert state[plugin] is False, plugin
    visible = call(runtime, "/api/plugins")["data"]["plugins"]
    assert set(REPO_TABS) <= set(visible)
    assert runtime.lookups == 0
    assert asyncio.run(plugin_data.account_package_installed(runtime, "bi:alice", "actions"))
    assert runtime.lookups == 0


def test_forkmesh_com_keeps_the_cutoff_for_new_accounts(hooks):
    for runtime in (Runtime(NEW), InstanceRuntime(NEW, name="  ")):
        state = installed(runtime)
        assert not any(state[plugin] for plugin in REPO_TABS)
        assert not set(REPO_TABS) & set(call(runtime, "/api/plugins")["data"]["plugins"])


def test_an_instance_account_can_still_remove_a_repository_tab(hooks):
    runtime = InstanceRuntime(NEW)
    call(runtime, "/api/plugins/@account/actions/uninstall", "POST")
    state = installed(runtime)
    assert state["actions"] is False and state["code"] is True
    assert "actions" not in call(runtime, "/api/plugins")["data"]["plugins"]
    assert not asyncio.run(plugin_data.account_package_installed(runtime, "bi:alice", "actions"))

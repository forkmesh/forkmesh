"""Shared organization lifecycle for market plugins.

Every market plugin goes through this one engine for organization installs so
"completely installed / completely removed" means the same thing everywhere:
installing a plugin creates that plugin's own tables (its migration), and
uninstalling always offers the organization's data as a download, purges every
row, and drops the plugin's schema once the last organization uninstalls —
reversing the plugin's migration so an uninstalled plugin leaves no trace in
the database. Plugins without server-side data participate with an install
registry row only.

The Worker adapter owns sessions, encryption, D1, and audit logging; per-plugin
data hooks live in the plugin's own module (DATA_SCHEMA_UP, DATA_SCHEMA_DOWN,
org_has_data, export_org_data, purge_org_data).

A package whose manifest declares ``dataLifecycle.scope: "account"`` is not
installed by organizations at all: each account installs it for itself through
the package's own API, and the package keeps that account's data private. This
engine only refuses organization installs for it, runs its reminder hook, and
purges an account's rows (``purge_account_data``) when the account is deleted.
"""

import importlib
import importlib.util
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

import instance_features
from plugin_registry import PLUGINS as REGISTRY


PREFIX = "/api/plugins"
MAX_BODY_BYTES = 64 * 1024
MANAGER_ROLES = frozenset({"owner", "admin"})
ORG_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
ACCOUNT_PATH = PREFIX + "/@account"
ACCOUNT_ACTIONS = ("install", "uninstall", "settings", "places")
PLACES = ("mobile", "desktop", "web")
ACCOUNT_SCHEMA_UP = (
    """CREATE TABLE IF NOT EXISTS account_package_installs (
        account_bi TEXT NOT NULL,
        package TEXT NOT NULL,
        installed INTEGER NOT NULL CHECK(installed IN (0, 1)),
        settings TEXT NOT NULL DEFAULT '{}',
        updated_at INTEGER NOT NULL DEFAULT 0 CHECK(updated_at >= 0),
        places TEXT NOT NULL DEFAULT '',
        PRIMARY KEY (account_bi, package))""",
)
ACCOUNT_SCHEMA_ALTER = (
    "ALTER TABLE account_package_installs ADD COLUMN places TEXT NOT NULL DEFAULT ''",
)
_ACCOUNT_SCHEMA_READY = False


def _module(plugin):
    """Load a plugin's Worker module (marketplace/packages/*/content/worker).

    The deploy stages every package's worker module next to this file, so the
    flat import succeeds on Cloudflare; local checkouts fall back to loading
    it straight from the package directory.
    """
    name = str(REGISTRY.get(plugin, {}).get("module") or "")
    if not name:
        return None
    try:
        return importlib.import_module(name)
    except ModuleNotFoundError:
        path = (Path(__file__).resolve().parents[2] / "marketplace" / "packages"
                / plugin / "content" / "worker" / (name + ".py"))
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
        return module


def _data_module(plugin):
    """The plugin's module when it declares organization-scoped server data."""
    if not REGISTRY.get(plugin, {}).get("organizationData"):
        return None
    return _module(plugin)


def _account_module(plugin):
    """The plugin's module when each account installs and owns its data."""
    if not REGISTRY.get(plugin, {}).get("accountData"):
        return None
    return _module(plugin)


def api_plugin_for(path):
    """The plugin id whose declared API prefix owns ``path``, or ""."""
    clean = str(path or "")
    for plugin in sorted(REGISTRY):
        prefix = str(REGISTRY[plugin].get("api") or "")
        if prefix and (clean == prefix or clean == prefix + "/"
                       or clean.startswith(prefix + "/")):
            return plugin
    return ""


def api_module(plugin):
    return _module(plugin) if str(REGISTRY.get(plugin, {}).get("api") or "") else None


def outbound_origins(plugin):
    """Exact manifest-declared origins available to a package Worker."""
    values = REGISTRY.get(plugin, {}).get("outboundOrigins") or ()
    return tuple(str(value) for value in values)


def permissions(plugin):
    return frozenset(str(value) for value in
                     REGISTRY.get(str(plugin or ""), {}).get("permissions") or ())


async def deliver_mailtrap_event(runtime, event, status, now):
    """Hand one authenticated Mailtrap event to every plugin webhook hook.

    A plugin whose schema was uninstalled (its tables dropped) must never fail
    the platform's webhook batch.
    """
    for plugin in sorted(REGISTRY):
        module = _module(plugin)
        hook = getattr(module, "mailtrap_webhook_event", None)
        if hook is None:
            continue
        try:
            await hook(runtime, event, status, now)
        except Exception as error:
            if "no such table" not in str(error).lower():
                raise


async def deliver_reminders(runtime):
    """Run every package's optional ``deliver_reminders(runtime)`` hook.

    A package whose schema is uninstalled (its tables dropped) must never fail
    the shared per-minute phase; every other error surfaces to the host.
    """
    delivered = 0
    for plugin in sorted(REGISTRY):
        module = _data_module(plugin) or _account_module(plugin)
        hook = getattr(module, "deliver_reminders", None)
        if hook is None:
            continue
        try:
            result = await hook(runtime)
        except Exception as error:
            if "no such table" not in str(error).lower():
                raise
            continue
        delivered += int((result or {}).get("delivered") or 0)
    return {"ok": True, "delivered": delivered}


async def deliver_lead(runtime, org_name, lead):
    for plugin in sorted(REGISTRY):
        module = _data_module(plugin)
        hook = getattr(module, "record_lead", None)
        if hook is None:
            continue
        try:
            result = await hook(runtime, org_name, lead)
        except Exception as error:
            if "no such table" not in str(error).lower():
                raise
            continue
        if isinstance(result, dict) and result.get("ok"):
            return dict(result, plugin=plugin)
    return {"ok": False, "status": "no_receiver"}


async def purge_account(runtime, account_bi, account_name):
    """Purge every account-scoped package's rows when the account is deleted.

    A package whose tables were never created must not fail the deletion.
    """
    for plugin in sorted(REGISTRY):
        module = _account_module(plugin)
        hook = getattr(module, "purge_account_data", None)
        if hook is None:
            continue
        try:
            await hook(runtime, account_bi, account_name)
        except Exception as error:
            if "no such table" not in str(error).lower():
                raise
    try:
        await runtime.d1_run(
            "DELETE FROM account_package_installs WHERE account_bi=?", account_bi)
    except Exception as error:
        if "no such table" not in str(error).lower():
            raise


def _response(runtime, payload, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        payload,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _route(path):
    clean = str(path or "").rstrip("/")
    match = re.fullmatch(
        r"/api/plugins/([a-z][a-z0-9-]{0,62})"
        r"(?:/([a-z][a-z0-9-]{0,62})/(install|uninstall|export))?",
        clean,
    )
    if not match:
        return None
    return match.group(1), match.group(2) or "", match.group(3) or ""


def _export_filename(plugin, org_name):
    return "forkmesh-" + plugin + "-" + org_name + ".json"


async def _installed_row(runtime, org_bi, plugin):
    return await runtime.d1_first(
        "SELECT plugin,installed_by_name,installed_at "
        "FROM org_plugin_installs WHERE org_bi=? AND plugin=?",
        org_bi, plugin,
    )


async def _install_count(runtime, plugin):
    row = await runtime.d1_first(
        "SELECT COUNT(*) AS installs FROM org_plugin_installs WHERE plugin=?",
        plugin,
    )
    return int((row or {}).get("installs") or 0)


async def _drop_schema_when_unused(runtime, plugin, module):
    """Reverse the plugin's migration once its last install is gone."""
    if module is None:
        return False
    if await _install_count(runtime, plugin):
        return False
    for statement in module.DATA_SCHEMA_DOWN:
        await runtime.d1_run(statement)
    # A concurrent install may have raced the drop; heal it so an organization
    # that just installed never loses its (empty) plugin schema.
    if await _install_count(runtime, plugin):
        for statement in module.DATA_SCHEMA_UP:
            await runtime.d1_run(statement)
        return False
    return True


async def _installed_has_data(runtime, plugin, module, org_bi):
    """Read one installed plugin defensively across package schema upgrades.

    An older installation can predate a table added by a newer package
    version. The shared lifecycle state lists every installed plugin after an
    install, so one stale schema must not make an unrelated package install
    fail. Heal only the precise missing-table case using that plugin's own
    idempotent schema and let every other database error surface normally.
    """
    try:
        return bool(await module.org_has_data(runtime, org_bi))
    except Exception as error:
        if "no such table" not in str(error).lower():
            raise
    for statement in module.DATA_SCHEMA_UP:
        await runtime.d1_run(statement)
    return bool(await module.org_has_data(runtime, org_bi))


async def _account_installed(runtime, plugin, account_bi):
    hook = getattr(_account_module(plugin), "account_installed", None)
    if hook is None:
        return False
    try:
        return bool(await hook(runtime, account_bi))
    except Exception as error:
        if "no such table" not in str(error).lower():
            raise
        return False


async def _shared_access(runtime, plugin, account_bi, account_name):
    hook = getattr(_data_module(plugin), "account_shared_access", None)
    if hook is None or not account_name:
        return False
    try:
        return bool(await hook(runtime, account_bi, account_name))
    except Exception as error:
        if "no such table" not in str(error).lower():
            raise
        return False


def _synced():
    return {plugin: entry for plugin, entry in REGISTRY.items()
            if entry.get("accountSync")}


def _requirements(plugin):
    seen = []
    pending = [plugin]
    while pending:
        current = pending.pop()
        for dependency in REGISTRY.get(current, {}).get("dependencies") or ():
            if dependency in REGISTRY and dependency not in seen and dependency != plugin:
                seen.append(dependency)
                pending.append(dependency)
    return seen


def _dependents(plugin):
    seen = []
    pending = [plugin]
    while pending:
        current = pending.pop()
        for candidate in sorted(REGISTRY):
            needs = REGISTRY[candidate].get("dependencies") or ()
            if current in needs and candidate not in seen and candidate != plugin:
                seen.append(candidate)
                pending.append(candidate)
    return seen


async def _ensure_account_schema(runtime):
    global _ACCOUNT_SCHEMA_READY
    if _ACCOUNT_SCHEMA_READY:
        return
    for statement in ACCOUNT_SCHEMA_UP:
        await runtime.d1_run(statement)
    for statement in ACCOUNT_SCHEMA_ALTER:
        try:
            await runtime.d1_run(statement)
        except Exception as error:
            if "duplicate column" not in str(error).lower():
                raise
    _ACCOUNT_SCHEMA_READY = True


async def _account_rows(runtime, account_bi):
    global _ACCOUNT_SCHEMA_READY
    await _ensure_account_schema(runtime)
    query = ("SELECT package,installed,settings,places FROM account_package_installs "
             "WHERE account_bi=?")
    try:
        rows = await runtime.d1_all(query, account_bi)
    except Exception as error:
        if "no such table" not in str(error).lower():
            raise
        _ACCOUNT_SCHEMA_READY = False
        await _ensure_account_schema(runtime)
        rows = await runtime.d1_all(query, account_bi)
    result = {}
    for row in rows or []:
        try:
            stored = json.loads(str(row.get("settings") or "{}"))
        except ValueError:
            stored = {}
        package = str(row.get("package") or "")
        result[package] = {
            "installed": bool(int(row.get("installed") or 0)),
            "settings": stored if isinstance(stored, dict) else {},
            "places": _stored_places(row.get("places")),
        }
    return result


def _available_places(plugin):
    declared = REGISTRY.get(plugin, {}).get("places") or PLACES
    return [place for place in PLACES if place in declared] or list(PLACES)


def _stored_places(value):
    return [place for place in PLACES if place in str(value or "").split(",")]


def _places(plugin, row):
    available = _available_places(plugin)
    chosen = [place for place in (row or {}).get("places") or () if place in available]
    return chosen or available


def _requested_places(plugin, data):
    if not isinstance(data, dict) or "places" not in data:
        return None, ""
    value = data.get("places")
    available = _available_places(plugin)
    if (not isinstance(value, list) or not value
            or any(not isinstance(place, str) or place not in available for place in value)):
        return None, "invalid_places"
    return [place for place in available if place in value], ""


def _cutoff_ms(value):
    try:
        moment = datetime.strptime(str(value), "%Y-%m-%d").replace(tzinfo=timezone.utc)
    except ValueError:
        return 0
    return int(moment.timestamp() * 1000)


def _instance_created_at(runtime, created_at):
    env = getattr(runtime, "env", None)
    return 0 if env is not None and instance_features.branded(env) else created_at


def _created_ms(created_at):
    try:
        return int(created_at or 0)
    except (TypeError, ValueError):
        return 0


def _legacy(plugin, created_at):
    cutoff = _cutoff_ms(REGISTRY.get(plugin, {}).get("installedBefore") or "")
    if not cutoff:
        return False
    created = _created_ms(created_at)
    return created <= 0 or created < cutoff


def _fallback_installed(plugin, created_at):
    return bool(REGISTRY.get(plugin, {}).get("default")) or _legacy(plugin, created_at)


def _hook_fallback(plugin):
    entry = REGISTRY.get(plugin, {})
    return (bool(entry.get("accountData")) and not entry.get("installedBefore")
            and callable(getattr(_account_module(plugin), "account_installed", None)))


def _targets(plugins):
    found = set()
    for plugin in plugins:
        if plugin in REGISTRY:
            found.add(plugin)
            found.update(_requirements(plugin))
    return sorted(found)


def _needs_created_at(plugins, rows):
    return any(plugin not in rows and REGISTRY.get(plugin, {}).get("installedBefore")
               and not REGISTRY.get(plugin, {}).get("default")
               for plugin in _targets(plugins))


async def _own_states(runtime, account_bi, rows, created_at, plugins=None):
    own = {}
    for plugin in _targets(sorted(_synced()) if plugins is None else plugins):
        row = rows.get(plugin)
        if row is not None:
            own[plugin] = row["installed"]
        elif _fallback_installed(plugin, created_at):
            own[plugin] = True
        elif _hook_fallback(plugin):
            own[plugin] = await _account_installed(runtime, plugin, account_bi)
        else:
            own[plugin] = False
    return own


def _effective_installed(plugin, own):
    if plugin not in REGISTRY or not own.get(plugin):
        return False
    return all(own.get(dependency) for dependency in _requirements(plugin))


def _setting_value(setting, value):
    if setting.get("type") == "boolean":
        return value if isinstance(value, bool) else None
    values = {str(option.get("value")) for option in setting.get("options") or ()}
    return value if isinstance(value, str) and value in values else None


def _settings_values(plugin, stored):
    values = {}
    for setting in REGISTRY.get(plugin, {}).get("settings") or ():
        value = _setting_value(setting, (stored or {}).get(setting.get("key")))
        values[setting["key"]] = setting.get("default") if value is None else value
    return values


def _legacy_only(plugin, rows, own):
    entry = REGISTRY.get(plugin, {})
    return (plugin not in rows and bool(own.get(plugin)) and not entry.get("default")
            and (bool(entry.get("installedBefore")) or _hook_fallback(plugin)))


def _unchosen(plugin, rows):
    entry = REGISTRY.get(plugin, {})
    return (plugin not in rows and not entry.get("default") and not entry.get("installedBefore")
            and not _hook_fallback(plugin))


def _account_payload(rows, own, changed=None):
    packages = []
    for plugin in sorted(_synced()):
        package = {
            "id": plugin,
            "installed": _effective_installed(plugin, own),
            "settings": _settings_values(plugin, (rows.get(plugin) or {}).get("settings")),
            "places": _places(plugin, rows.get(plugin)),
            "availablePlaces": _available_places(plugin),
        }
        if _legacy_only(plugin, rows, own):
            package["legacy"] = True
        elif _unchosen(plugin, rows):
            package["unchosen"] = True
        packages.append(package)
    payload = {"ok": True, "packages": packages}
    if changed is not None:
        payload["changed"] = changed
    return payload


async def account_package_installed(runtime, account_bi, plugin, created_at=None):
    if not account_bi or plugin not in _synced():
        return False
    rows = await _account_rows(runtime, account_bi)
    created_at = _instance_created_at(runtime, created_at)
    lookup = getattr(runtime, "account_created_at", None)
    if created_at is None and lookup is not None and _needs_created_at([plugin], rows):
        created_at = await lookup(account_bi)
    own = await _own_states(runtime, account_bi, rows, created_at, [plugin])
    return _effective_installed(plugin, own)


async def account_package_settings(runtime, account_bi, plugin):
    rows = await _account_rows(runtime, account_bi) if account_bi else {}
    return _settings_values(plugin, (rows.get(plugin) or {}).get("settings"))


async def _store_account_row(runtime, account_bi, plugin, installed, settings, places=()):
    chosen = [place for place in _available_places(plugin) if place in (places or ())]
    await runtime.d1_run(
        "INSERT INTO account_package_installs(account_bi,package,installed,"
        "settings,updated_at,places) VALUES(?,?,?,?,?,?) ON CONFLICT(account_bi,package) "
        "DO UPDATE SET installed=excluded.installed,settings=excluded.settings,"
        "updated_at=excluded.updated_at,places=excluded.places",
        account_bi, plugin, 1 if installed else 0,
        json.dumps(settings or {}, sort_keys=True, separators=(",", ":")),
        runtime.now(), ",".join(chosen),
    )


def _changed(before, after):
    return [plugin for plugin in sorted(_synced())
            if _effective_installed(plugin, before) != _effective_installed(plugin, after)]


async def _account_state(runtime, account_bi, created_at):
    rows = await _account_rows(runtime, account_bi)
    return rows, await _own_states(runtime, account_bi, rows, created_at)


async def _account_install(runtime, account_bi, actor, plugin, created_at, places=None):
    before, before_own = await _account_state(runtime, account_bi, created_at)
    for target in [plugin] + _requirements(plugin):
        row = before.get(target) or {}
        if target not in _synced():
            continue
        if row.get("installed") and (target != plugin or places is None):
            continue
        chosen = row.get("places") or ()
        if places is not None and (target == plugin or not row.get("installed")):
            chosen = places
        await _store_account_row(
            runtime, account_bi, target, True, row.get("settings") or {}, chosen)
    after, after_own = await _account_state(runtime, account_bi, created_at)
    await runtime.audit(actor, "plugin.install", "account_plugin", actor + ":" + plugin)
    return _response(runtime, _account_payload(
        after, after_own, _changed(before_own, after_own)))


async def _purge_account_package(runtime, account_bi, actor, plugin):
    hook = getattr(_account_module(plugin), "purge_account_data", None)
    if hook is None:
        return
    try:
        await hook(runtime, account_bi, actor)
    except Exception as error:
        if "no such table" not in str(error).lower():
            raise


async def _delete_account_row(runtime, account_bi, plugin):
    await runtime.d1_run(
        "DELETE FROM account_package_installs WHERE account_bi=? AND package=?",
        account_bi, plugin)


async def _account_uninstall(runtime, account_bi, actor, plugin, created_at):
    before, before_own = await _account_state(runtime, account_bi, created_at)
    dependents = [item for item in _dependents(plugin) if item in _synced()]
    if _hook_fallback(plugin):
        await _delete_account_row(runtime, account_bi, plugin)
    else:
        await _store_account_row(runtime, account_bi, plugin, False, {})
    for dependent in dependents:
        await _delete_account_row(runtime, account_bi, dependent)
    for target in [plugin] + dependents:
        await _purge_account_package(runtime, account_bi, actor, target)
    after, after_own = await _account_state(runtime, account_bi, created_at)
    await runtime.audit(actor, "plugin.uninstall", "account_plugin", actor + ":" + plugin)
    return _response(runtime, _account_payload(
        after, after_own, _changed(before_own, after_own)))


async def _account_save_settings(runtime, account_bi, actor, plugin, data, created_at):
    incoming = data.get("settings") if isinstance(data, dict) else None
    if not isinstance(incoming, dict):
        return _response(runtime, {"error": "invalid_setting", "key": ""}, 400)
    declared = {setting["key"]: setting
                for setting in REGISTRY.get(plugin, {}).get("settings") or ()}
    rows, own = await _account_state(runtime, account_bi, created_at)
    current = _settings_values(plugin, (rows.get(plugin) or {}).get("settings"))
    for key, value in incoming.items():
        setting = declared.get(str(key))
        if setting is None or _setting_value(setting, value) is None:
            return _response(runtime, {"error": "invalid_setting", "key": str(key)[:40]}, 400)
        current[setting["key"]] = value
    await _store_account_row(
        runtime, account_bi, plugin, bool(own.get(plugin)), current,
        (rows.get(plugin) or {}).get("places"))
    await runtime.audit(actor, "plugin.settings", "account_plugin", actor + ":" + plugin)
    return _response(runtime, _account_payload(
        *await _account_state(runtime, account_bi, created_at)))


async def _account_save_places(runtime, account_bi, actor, plugin, places, created_at):
    rows, own = await _account_state(runtime, account_bi, created_at)
    if not _effective_installed(plugin, own):
        return _response(runtime, {"error": "plugin_not_installed"}, 409)
    row = rows.get(plugin) or {}
    await _store_account_row(runtime, account_bi, plugin, True, row.get("settings") or {}, places)
    await runtime.audit(actor, "plugin.places", "account_plugin", actor + ":" + plugin)
    return _response(runtime, _account_payload(
        *await _account_state(runtime, account_bi, created_at)))


def _account_route(path):
    clean = str(path or "").rstrip("/")
    if clean == ACCOUNT_PATH:
        return "", ""
    match = re.fullmatch(
        re.escape(ACCOUNT_PATH) + r"/([a-z][a-z0-9-]{0,62})/(install|uninstall|settings|places)",
        clean)
    if not match:
        return None
    return match.group(1), match.group(2)


async def _handle_account(runtime, path):
    route = _account_route(path)
    if route is None:
        return _response(runtime, {"error": "not_found"}, 404)
    plugin, action = route
    method = runtime.method()
    allowed = ("POST",) if action else ("GET",)
    if method not in allowed:
        return _response(
            runtime, {"error": "method_not_allowed"}, 405, ", ".join(allowed))
    if method not in ("GET", "HEAD") and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, 403)
    data = {}
    if method not in ("GET", "HEAD"):
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _response(
                runtime, {"error": error},
                413 if error == "payload_too_large" else 400,
            )
    await runtime.ensure_schema()
    account_bi, record = await runtime.session(data)
    actor = " ".join(str((record or {}).get("name") or "").split())[:64].lower()
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    account_bi = str(account_bi)
    created_at = _instance_created_at(runtime, (record or {}).get("created_at"))
    if not action:
        return _response(runtime, _account_payload(
            *await _account_state(runtime, account_bi, created_at)))
    if plugin not in REGISTRY:
        return _response(runtime, {"error": "plugin_not_found"}, 404)
    if plugin not in _synced():
        return _response(runtime, {"error": "plugin_not_account_synced"}, 409)
    places, error = _requested_places(plugin, data)
    if error or (action == "places" and places is None):
        return _response(runtime, {"error": error or "invalid_places"}, 400)
    if action == "install":
        return await _account_install(runtime, account_bi, actor, plugin, created_at, places)
    if action == "uninstall":
        return await _account_uninstall(runtime, account_bi, actor, plugin, created_at)
    if action == "places":
        return await _account_save_places(
            runtime, account_bi, actor, plugin, places, created_at)
    return await _account_save_settings(
        runtime, account_bi, actor, plugin, data, created_at)


async def _visible(runtime, account_bi, account_name="", created_at=None):
    rows = await runtime.d1_all(
        "SELECT DISTINCT i.plugin AS plugin FROM org_plugin_installs i "
        "JOIN org_members m ON m.org_bi=i.org_bi WHERE m.member_bi=?",
        account_bi,
    )
    org_installed = {str(row.get("plugin") or "") for row in rows or []}
    member = None
    plugins = []
    synced_rows, synced_own = {}, {}
    if _synced():
        synced_rows, synced_own = await _account_state(runtime, account_bi, created_at)
    for plugin in sorted(REGISTRY):
        entry = REGISTRY[plugin]
        if entry.get("accountSync"):
            visible = (_effective_installed(plugin, synced_own)
                       and "web" in _places(plugin, synced_rows.get(plugin)))
        elif entry.get("accountData"):
            visible = await _account_installed(runtime, plugin, account_bi)
        elif entry.get("organizationData"):
            visible = plugin in org_installed
            if not visible and entry.get("default"):
                if member is None:
                    member = bool(await runtime.d1_first(
                        "SELECT 1 AS present FROM org_members "
                        "WHERE member_bi=? LIMIT 1", account_bi))
                visible = member
            if not visible:
                visible = await _shared_access(
                    runtime, plugin, account_bi, account_name)
        else:
            visible = plugin in org_installed or _fallback_installed(plugin, created_at)
        if visible:
            plugins.append(plugin)
    return _response(runtime, {"ok": True, "plugins": plugins})


async def _state(runtime, org_bi, org_name, role):
    rows = await runtime.d1_all(
        "SELECT plugin,installed_by_name,installed_at FROM org_plugin_installs "
        "WHERE org_bi=? ORDER BY plugin",
        org_bi,
    )
    installed = {str(row.get("plugin") or ""): row for row in rows or []}
    plugins = []
    for plugin in sorted(REGISTRY):
        if REGISTRY[plugin].get("accountData") or REGISTRY[plugin].get("accountSync"):
            continue
        row = installed.get(plugin)
        module = _data_module(plugin)
        plugins.append({
            "id": plugin,
            "installed": bool(row),
            "installedBy": str((row or {}).get("installed_by_name") or ""),
            "installedAt": int((row or {}).get("installed_at") or 0),
            "serverData": module is not None,
            "hasData": bool(
                row and module is not None
                and await _installed_has_data(
                    runtime, plugin, module, org_bi)),
        })
    return _response(runtime, {
        "ok": True,
        "organization": org_name,
        "role": role,
        "canManage": role in MANAGER_ROLES,
        "plugins": plugins,
    })


async def _install(runtime, org_bi, org_name, account_bi, actor, role, plugin):
    module = _data_module(plugin)
    if module is not None:
        for statement in module.DATA_SCHEMA_UP:
            await runtime.d1_run(statement)
    await runtime.d1_run(
        "INSERT INTO org_plugin_installs(org_bi,plugin,org_name,"
        "installed_by_bi,installed_by_name,installed_at) VALUES(?,?,?,?,?,?) "
        "ON CONFLICT(org_bi,plugin) DO NOTHING",
        org_bi, plugin, org_name, account_bi, actor, runtime.now(),
    )
    await runtime.audit(
        actor, "plugin.install", "organization_plugin", org_name + ":" + plugin)
    return await _state(runtime, org_bi, org_name, role)


async def _uninstall(runtime, org_bi, org_name, actor, plugin):
    row = await _installed_row(runtime, org_bi, plugin)
    if not row:
        return _response(runtime, {"error": "plugin_not_installed"}, 409)
    module = _data_module(plugin)
    export = None
    if module is not None and await module.org_has_data(runtime, org_bi):
        export = await module.export_org_data(runtime, org_bi, org_name)
    if module is not None:
        await module.purge_org_data(runtime, org_bi)
    await runtime.d1_run(
        "DELETE FROM org_plugin_installs WHERE org_bi=? AND plugin=?",
        org_bi, plugin,
    )
    dropped = await _drop_schema_when_unused(runtime, plugin, module)
    await runtime.audit(
        actor, "plugin.uninstall", "organization_plugin",
        org_name + ":" + plugin)
    return _response(runtime, {
        "ok": True,
        "uninstalled": True,
        "plugin": plugin,
        "organization": org_name,
        "hadData": export is not None,
        "schemaDropped": dropped,
        "filename": _export_filename(plugin, org_name),
        "export": export,
    })


async def _export(runtime, org_bi, org_name, plugin):
    if not await _installed_row(runtime, org_bi, plugin):
        return _response(runtime, {"error": "plugin_not_installed"}, 409)
    module = _data_module(plugin)
    if module is None:
        return _response(runtime, {"error": "plugin_has_no_server_data"}, 409)
    return _response(runtime, {
        "ok": True,
        "plugin": plugin,
        "organization": org_name,
        "hasData": await module.org_has_data(runtime, org_bi),
        "filename": _export_filename(plugin, org_name),
        "export": await module.export_org_data(runtime, org_bi, org_name),
    })


async def purge_org(runtime, org_bi):
    """Purge every installed plugin's data when an organization is deleted.

    Reverses each plugin's schema too once the deleted organization was its
    last install, exactly like an explicit uninstall.
    """
    rows = await runtime.d1_all(
        "SELECT plugin FROM org_plugin_installs WHERE org_bi=?", org_bi)
    for row in rows or []:
        plugin = str((row or {}).get("plugin") or "")
        module = _data_module(plugin) if plugin in REGISTRY else None
        if module is not None:
            await module.purge_org_data(runtime, org_bi)
        await runtime.d1_run(
            "DELETE FROM org_plugin_installs WHERE org_bi=? AND plugin=?",
            org_bi, plugin,
        )
        await _drop_schema_when_unused(runtime, plugin, module)


async def handle(runtime, path):
    clean = str(path or "").rstrip("/")
    if clean == ACCOUNT_PATH or clean.startswith(ACCOUNT_PATH + "/"):
        return await _handle_account(runtime, path)
    if str(path or "").rstrip("/") == PREFIX:
        if runtime.method() != "GET":
            return _response(
                runtime, {"error": "method_not_allowed"}, 405, "GET")
        await runtime.ensure_schema()
        account_bi, record = await runtime.session({})
        if not account_bi or not record:
            return _response(runtime, {"error": "invalid_session"}, 401)
        name = " ".join(str(record.get("name") or "").split())[:64].lower()
        return await _visible(runtime, str(account_bi), name,
                              _instance_created_at(runtime, record.get("created_at")))
    route = _route(path)
    if route is None:
        return _response(runtime, {"error": "not_found"}, 404)
    org_name, plugin, action = route
    method = runtime.method()
    allowed = ("POST",) if action in ("install", "uninstall") else ("GET",)
    if method not in allowed:
        return _response(
            runtime, {"error": "method_not_allowed"}, 405, ", ".join(allowed))
    if method not in ("GET", "HEAD") and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, 403)
    data = {}
    if method not in ("GET", "HEAD"):
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _response(
                runtime, {"error": error},
                413 if error == "payload_too_large" else 400,
            )
    await runtime.ensure_schema()
    account_bi, record = await runtime.session(data)
    actor = " ".join(str((record or {}).get("name") or "").split())[:64].lower()
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    if not ORG_RE.fullmatch(org_name):
        return _response(runtime, {"error": "organization_not_found"}, 404)
    org_bi, role = await runtime.organization(org_name, str(account_bi))
    if not org_bi or not role:
        return _response(runtime, {"error": "organization_not_found"}, 404)
    if not plugin:
        return await _state(runtime, org_bi, org_name, role)
    if plugin not in REGISTRY:
        return _response(runtime, {"error": "plugin_not_found"}, 404)
    if role not in MANAGER_ROLES:
        return _response(runtime, {"error": "organization_admin_required"}, 403)
    if action == "install" and (REGISTRY[plugin].get("accountData")
                                or REGISTRY[plugin].get("accountSync")):
        return _response(runtime, {"error": "plugin_installs_per_account"}, 409)
    if action == "install":
        return await _install(
            runtime, org_bi, org_name, str(account_bi), actor, role, plugin)
    if action == "uninstall":
        return await _uninstall(runtime, org_bi, org_name, actor, plugin)
    return await _export(runtime, org_bi, org_name, plugin)

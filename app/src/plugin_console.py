import json
import re
from pathlib import Path

import market_packages
import plugin_data


PATH = plugin_data.CONSOLE_PATH
LOG_WINDOW_MS = 30 * 24 * 3600 * 1000
LOG_LIMIT = 100
ROWS_LIMIT = 100
VALUE_LIMIT = 400
EXPORT_ROWS = 5000
EXPORT_BYTES = 8 * 1024 * 1024
TABLE_RE = re.compile(
    r"CREATE\s+(?:VIRTUAL\s+)?TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?[\"`]?([A-Za-z_][A-Za-z0-9_]*)",
    re.I)
NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,95}$")
CORE_TABLES = {
    "account_package_installs": "package",
    "org_plugin_installs": "plugin",
}
_CORE_SCHEMA = None
_MODULE_TABLES = {}


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PATH:
        return "", ""
    match = re.fullmatch(
        re.escape(PATH) + r"/([a-z][a-z0-9.-]{0,95})(?:/(rows|export|policy))?", clean)
    if not match:
        return None
    return match.group(1), match.group(2) or ""


def _missing_table(error):
    return "no such table" in str(error).lower()


async def _all(runtime, sql, *args):
    try:
        return await runtime.d1_all(sql, *args) or []
    except Exception as error:
        if _missing_table(error):
            return []
        raise


def _core_schema_tables():
    global _CORE_SCHEMA
    if _CORE_SCHEMA is None:
        try:
            import schema
            text = "\n".join(str(item) for item in schema.SCHEMA_STATEMENTS)
        except Exception:
            text = ""
        _CORE_SCHEMA = frozenset(name.lower() for name in TABLE_RE.findall(text))
    return _CORE_SCHEMA


def _catalog():
    rows = market_packages.payload().get("packages") or []
    facts = {plugin: dict(entry.get("facts") or {})
             for plugin, entry in plugin_data.REGISTRY.items() if entry.get("facts")}
    if not facts or any(row.get("id") not in facts for row in rows):
        for plugin, value in market_packages.catalog_facts().items():
            facts.setdefault(plugin, value)
    return rows, facts


def _module_tables(plugin):
    if plugin in _MODULE_TABLES:
        return _MODULE_TABLES[plugin]
    names = []
    texts = []
    module = plugin_data._module(plugin) if plugin in plugin_data.REGISTRY else None
    for attribute in dir(module) if module is not None else ():
        value = getattr(module, attribute, None)
        if "SCHEMA" in attribute.upper() and isinstance(value, (str, list, tuple)):
            texts.append(value if isinstance(value, str) else "\n".join(
                str(item) for item in value))
    source = str(getattr(module, "__file__", "") or "")
    if source:
        try:
            texts.append(Path(source).read_text(encoding="utf-8"))
        except OSError:
            pass
    core = _core_schema_tables()
    for text in texts:
        for table in TABLE_RE.findall(text):
            if table not in names and table.lower() not in core and table not in CORE_TABLES:
                names.append(table)
    _MODULE_TABLES[plugin] = tuple(sorted(names))
    return _MODULE_TABLES[plugin]


def _quote(name):
    return '"' + str(name).replace('"', '""') + '"'


async def _existing(runtime, names):
    if not names:
        return set()
    rows = await _all(
        runtime,
        "SELECT name FROM sqlite_master WHERE type='table' AND name IN ("
        + ",".join("?" for _ in names) + ")", *names)
    return {str(row.get("name") or "") for row in rows}


async def _columns(runtime, table):
    rows = await _all(runtime, "PRAGMA table_info(" + _quote(table) + ")")
    return [str(row.get("name") or "") for row in rows if row.get("name")]


def _owned(plugin):
    tables = [{"name": name, "owner": "package", "filter": ""}
              for name in _module_tables(plugin)]
    entry = plugin_data.REGISTRY.get(plugin, {})
    if entry.get("accountSync"):
        tables.append({"name": "account_package_installs", "owner": "core",
                       "filter": "package"})
    if not entry.get("accountSync") and not entry.get("accountData"):
        tables.append({"name": "org_plugin_installs", "owner": "core", "filter": "plugin"})
    return tables


def _where(table, plugin):
    if table["filter"]:
        return " WHERE " + _quote(table["filter"]) + "=?", (plugin,)
    return "", ()


async def _table_stats(runtime, plugin):
    tables = _owned(plugin)
    present = await _existing(runtime, [table["name"] for table in tables])
    result = []
    for table in tables:
        name = table["name"]
        row = {"name": name, "owner": table["owner"], "exists": name in present,
               "rows": 0, "bytes": 0, "columns": []}
        if name in present:
            columns = await _columns(runtime, name)
            where, args = _where(table, plugin)
            size = "+".join("COALESCE(LENGTH(CAST(" + _quote(column) + " AS BLOB)),0)"
                            for column in columns) or "0"
            stats = await runtime.d1_first(
                "SELECT COUNT(*) AS n, COALESCE(SUM(" + size + "),0) AS b FROM "
                + _quote(name) + where, *args) or {}
            row.update(rows=int(stats.get("n") or 0), bytes=int(stats.get("b") or 0),
                       columns=columns)
        result.append(row)
    return result


async def _install_counts(runtime):
    accounts = {}
    for row in await _all(
            runtime,
            "SELECT package, SUM(installed) AS on_count, COUNT(*) AS total "
            "FROM account_package_installs GROUP BY package"):
        on = int(row.get("on_count") or 0)
        accounts[str(row.get("package") or "")] = {
            "accounts": on, "removed": int(row.get("total") or 0) - on}
    organizations = {}
    for row in await _all(
            runtime,
            "SELECT plugin, COUNT(*) AS n FROM org_plugin_installs GROUP BY plugin"):
        organizations[str(row.get("plugin") or "")] = int(row.get("n") or 0)
    return accounts, organizations


async def _mine(runtime, account_bi, record):
    if not plugin_data._synced():
        return {}
    created_at = plugin_data._instance_created_at(runtime, (record or {}).get("created_at"))
    rows, own = await plugin_data._account_state(runtime, account_bi, created_at)
    return {plugin: {
        "installed": plugin_data._effective_installed(plugin, own),
        "places": plugin_data._places(plugin, rows.get(plugin)),
    } for plugin in plugin_data._synced()}


def _policy(plugin, row):
    rule = plugin_data.policy(plugin)
    manifest = [place for place in market_packages.PLACES
                if place in (row.get("places") or market_packages.PLACES)]
    if rule is None:
        return {"restricted": False, "disabled": False, "places": manifest,
                "updatedBy": "", "updatedAt": 0}
    allowed = [place for place in manifest if place in rule["places"]]
    return {"restricted": True, "disabled": not allowed, "places": allowed,
            "updatedBy": rule["updatedBy"], "updatedAt": rule["updatedAt"]}


def _package(row, facts, accounts, organizations, mine):
    plugin = row["id"]
    entry = plugin_data.REGISTRY.get(plugin, {})
    fact = facts.get(plugin) or {}
    surfaces = row.get("surfaces") or {}
    return {
        "id": plugin,
        "name": row.get("name") or plugin,
        "version": row.get("version") or "",
        "summary": row.get("summary") or "",
        "category": row.get("category") or "",
        "kind": fact.get("kind") or "",
        "inBuild": plugin in plugin_data.REGISTRY if plugin_data.REGISTRY else None,
        "surfaces": {
            "worker": fact.get("module") or entry.get("module") or "",
            "api": row.get("api") or "",
            "dashboard": fact.get("dashboard") or "",
            "www": fact.get("www") or "",
            "desktop": fact.get("desktop", "module" if surfaces.get("desktop") else ""),
            "world": bool(fact.get("world") or surfaces.get("world")),
        },
        "places": list(row.get("places") or market_packages.PLACES),
        "policy": _policy(plugin, row),
        "accountSync": bool(row.get("accountSync")),
        "dataScope": row.get("dataScope") or "",
        "default": bool(row.get("default")),
        "installedBefore": row.get("installedBefore") or "",
        "dependencies": [item.get("id") for item in row.get("dependencies") or []
                         if isinstance(item, dict)],
        "installs": {
            "accounts": (accounts.get(plugin) or {}).get("accounts", 0),
            "removed": (accounts.get(plugin) or {}).get("removed", 0),
            "organizations": organizations.get(plugin, 0),
        },
        "mine": mine.get(plugin),
    }


async def _list(runtime, account_bi, record):
    await plugin_data.load_policy(runtime, fresh=True)
    rows, facts = _catalog()
    accounts, organizations = await _install_counts(runtime)
    mine = await _mine(runtime, account_bi, record)
    packages = [_package(row, facts, accounts, organizations, mine) for row in rows]
    return plugin_data._response(runtime, {
        "ok": True,
        "staged": bool(plugin_data.REGISTRY),
        "places": list(market_packages.PLACES),
        "count": len(packages),
        "packages": packages,
    })


def _find(plugin):
    rows, facts = _catalog()
    for row in rows:
        if row.get("id") == plugin:
            return row, facts
    return None, facts


def _prefixes(package):
    surfaces = package["surfaces"]
    values = [surfaces["api"], plugin_data.PREFIX + "/" + package["id"],
              surfaces["dashboard"], surfaces["www"]]
    return [value for value in dict.fromkeys(values) if value]


async def _errors(runtime, package):
    prefixes = _prefixes(package)
    clauses = " OR ".join("path=? OR path LIKE ?" for _ in prefixes)
    args = []
    for prefix in prefixes:
        args.extend((prefix, prefix + "/%"))
    rows = await _all(
        runtime,
        "SELECT id, ts, status, method, path, message, actor FROM error_log "
        "WHERE ts>=? AND (" + clauses + ") ORDER BY id DESC LIMIT ?",
        runtime.now() - LOG_WINDOW_MS, *args, LOG_LIMIT)
    return [{
        "id": int(row.get("id") or 0), "ts": int(row.get("ts") or 0),
        "status": int(row.get("status") or 0), "method": str(row.get("method") or ""),
        "path": str(row.get("path") or ""), "message": str(row.get("message") or "")[:600],
        "actor": str(row.get("actor") or ""),
    } for row in rows]


async def _activity(runtime, plugin):
    rows = await _all(
        runtime,
        "SELECT id, ts, actor_label, action, target_type, outcome FROM sensitive_audit_log "
        "WHERE ts>=? AND action LIKE 'plugin.%' AND details LIKE ? ORDER BY id DESC LIMIT ?",
        runtime.now() - LOG_WINDOW_MS,
        "%" + json.dumps({"package": plugin}, separators=(",", ":"))[1:-1] + "%",
        LOG_LIMIT)
    return [{
        "id": int(row.get("id") or 0), "ts": int(row.get("ts") or 0),
        "actor": str(row.get("actor_label") or ""), "action": str(row.get("action") or ""),
        "scope": str(row.get("target_type") or ""), "outcome": str(row.get("outcome") or ""),
    } for row in rows]


async def _installs(runtime, plugin):
    places = {place: 0 for place in market_packages.PLACES}
    available = plugin_data._available_places(plugin)
    for row in await _all(
            runtime,
            "SELECT places, COUNT(*) AS n FROM account_package_installs "
            "WHERE package=? AND installed=1 GROUP BY places", plugin):
        chosen = [place for place in plugin_data._stored_places(row.get("places"))
                  if place in available] or available
        for place in chosen:
            places[place] += int(row.get("n") or 0)
    organizations = [{
        "name": str(row.get("org_name") or ""),
        "installedBy": str(row.get("installed_by_name") or ""),
        "installedAt": int(row.get("installed_at") or 0),
    } for row in await _all(
        runtime,
        "SELECT org_name, installed_by_name, installed_at FROM org_plugin_installs "
        "WHERE plugin=? ORDER BY installed_at DESC LIMIT 200", plugin)]
    return {"places": places, "organizations": organizations}


async def _detail(runtime, account_bi, record, plugin):
    row, facts = _find(plugin)
    if row is None:
        return plugin_data._response(runtime, {"error": "plugin_not_found"}, 404)
    await plugin_data.load_policy(runtime, fresh=True)
    accounts, organizations = await _install_counts(runtime)
    package = _package(row, facts, accounts, organizations,
                       await _mine(runtime, account_bi, record))
    entry = plugin_data.REGISTRY.get(plugin, {})
    package.update(
        settings=list(row.get("settings") or []),
        permissions=list(row.get("permissions") or []),
        outboundOrigins=list(entry.get("outboundOrigins") or ()),
        dependents=[item for item in plugin_data._dependents(plugin)],
        monitors=row.get("monitors") or {},
        publisher=row.get("publisher") or "",
    )
    tables = await _table_stats(runtime, plugin)
    return plugin_data._response(runtime, {
        "ok": True,
        "package": package,
        "tables": tables,
        "storage": {
            "tables": sum(1 for table in tables if table["exists"]),
            "rows": sum(table["rows"] for table in tables),
            "bytes": sum(table["bytes"] for table in tables),
        },
        "installs": await _installs(runtime, plugin),
        "logs": {
            "windowDays": LOG_WINDOW_MS // (24 * 3600 * 1000),
            "prefixes": _prefixes(package),
            "errors": await _errors(runtime, package),
            "activity": await _activity(runtime, plugin),
        },
    })


def _cell(value, limit=VALUE_LIMIT):
    if value is None or isinstance(value, (bool, int, float)):
        return value
    if isinstance(value, (bytes, bytearray, memoryview)):
        return "<" + str(len(bytes(value))) + " bytes>"
    text = str(value)
    return text if len(text) <= limit else text[:limit] + "…"


def _int(value, default, low, high):
    try:
        number = int(value)
    except (TypeError, ValueError):
        return default
    return max(low, min(high, number))


def _table(plugin, name):
    for table in _owned(plugin):
        if table["name"] == name and NAME_RE.fullmatch(name):
            return table
    return None


async def _rows(runtime, actor, plugin):
    query = runtime.query_params()
    table = _table(plugin, str(query.get("table") or ""))
    if table is None:
        return plugin_data._response(runtime, {"error": "table_not_found"}, 404)
    if table["name"] not in await _existing(runtime, [table["name"]]):
        return plugin_data._response(runtime, {"error": "table_not_created"}, 409)
    limit = _int(query.get("limit"), 50, 1, ROWS_LIMIT)
    offset = _int(query.get("offset"), 0, 0, 10 ** 9)
    columns = await _columns(runtime, table["name"])
    where, args = _where(table, plugin)
    total = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM " + _quote(table["name"]) + where, *args) or {}
    rows = await _all(
        runtime,
        "SELECT * FROM " + _quote(table["name"]) + where + " ORDER BY rowid DESC LIMIT ? OFFSET ?",
        *args, limit, offset)
    await runtime.audit(actor, "plugin.console.rows", "package_console", plugin,
                        details={"package": plugin, "table": table["name"]})
    return plugin_data._response(runtime, {
        "ok": True,
        "plugin": plugin,
        "table": table["name"],
        "columns": columns,
        "rows": [[_cell(row.get(column)) for column in columns] for row in rows],
        "offset": offset,
        "limit": limit,
        "total": int(total.get("n") or 0),
    })


async def _export(runtime, actor, plugin):
    row, _ = _find(plugin)
    if row is None:
        return plugin_data._response(runtime, {"error": "plugin_not_found"}, 404)
    tables = _owned(plugin)
    present = await _existing(runtime, [table["name"] for table in tables])
    exported = {}
    budget = EXPORT_BYTES
    truncated = False
    for table in tables:
        if table["name"] not in present:
            continue
        columns = await _columns(runtime, table["name"])
        where, args = _where(table, plugin)
        rows = await _all(
            runtime,
            "SELECT * FROM " + _quote(table["name"]) + where + " ORDER BY rowid LIMIT ?",
            *args, EXPORT_ROWS + 1)
        if len(rows) > EXPORT_ROWS:
            rows = rows[:EXPORT_ROWS]
            truncated = True
        values = []
        for item in rows:
            line = [_cell(item.get(column), EXPORT_BYTES) for column in columns]
            budget -= len(json.dumps(line, default=str))
            if budget < 0:
                truncated = True
                break
            values.append(line)
        exported[table["name"]] = {"owner": table["owner"], "columns": columns, "rows": values}
        if budget < 0:
            break
    await runtime.audit(actor, "plugin.console.export", "package_console", plugin,
                        details={"package": plugin, "truncated": truncated})
    return plugin_data._response(runtime, {
        "ok": True,
        "plugin": plugin,
        "version": row.get("version") or "",
        "exportedAt": runtime.now(),
        "filename": "forkmesh-" + plugin + "-tables.json",
        "truncated": truncated,
        "tables": exported,
    })


async def _save_policy(runtime, actor, plugin, data):
    row, _ = _find(plugin)
    if row is None:
        return plugin_data._response(runtime, {"error": "plugin_not_found"}, 404)
    manifest = [place for place in market_packages.PLACES
                if place in (row.get("places") or market_packages.PLACES)]
    if data.get("reset") is True:
        places = None
    else:
        places = data.get("places")
        if (not isinstance(places, list)
                or any(not isinstance(place, str) or place not in manifest for place in places)):
            return plugin_data._response(runtime, {"error": "invalid_places"}, 400)
        places = [place for place in manifest if place in places]
        if places == manifest:
            places = None
    await plugin_data.save_policy(runtime, plugin, places, actor)
    rule = _policy(plugin, row)
    await runtime.audit(actor, "plugin.console.policy", "package_console", plugin,
                        details={"package": plugin, "places": ",".join(rule["places"]),
                                 "disabled": rule["disabled"]})
    return plugin_data._response(runtime, {"ok": True, "plugin": plugin, "policy": rule})


async def handle(runtime, path):
    route = _route(path)
    if route is None:
        return plugin_data._response(runtime, {"error": "not_found"}, 404)
    allowed = ("POST",) if route[1] == "policy" else ("GET", "HEAD")
    if runtime.method() not in allowed:
        return plugin_data._response(
            runtime, {"error": "method_not_allowed"}, 405, ", ".join(allowed))
    data = {}
    if route[1] == "policy":
        if not runtime.same_origin():
            return plugin_data._response(runtime, {"error": "origin_not_allowed"}, 403)
        data, error = await runtime.json_body(plugin_data.MAX_BODY_BYTES)
        if error:
            return plugin_data._response(
                runtime, {"error": error}, 413 if error == "payload_too_large" else 400)
    await runtime.ensure_schema()
    account_bi, record = await runtime.session(data)
    actor = " ".join(str((record or {}).get("name") or "").split())[:64].lower()
    if not account_bi or not actor:
        return plugin_data._response(runtime, {"error": "invalid_session"}, 401)
    if not await runtime.is_admin(actor):
        return plugin_data._response(runtime, {"error": "admin_required"}, 403)
    await plugin_data._ensure_account_schema(runtime)
    plugin, action = route
    if not plugin:
        return await _list(runtime, str(account_bi), record)
    if action == "rows":
        if _find(plugin)[0] is None:
            return plugin_data._response(runtime, {"error": "plugin_not_found"}, 404)
        return await _rows(runtime, actor, plugin)
    if action == "export":
        return await _export(runtime, actor, plugin)
    if action == "policy":
        return await _save_policy(runtime, actor, plugin, data)
    return await _detail(runtime, str(account_bi), record, plugin)

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2] / "marketplace"
MARKET = "forkmesh"
ID_RE = re.compile(r"^(?=.{1,96}$)[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*$")
VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]{1,40})?$")
ROUTE_RE = re.compile(r"^/dashboard/[a-z0-9/-]{1,80}$")
SITE_ROUTE_RE = re.compile(r"^/[a-z][a-z0-9-]{0,39}$")
API_RE = re.compile(r"^/api/[a-z][a-z0-9-]{0,62}$")
INSTALLED_BEFORE_RE = re.compile(r"^20[0-9]{2}-(?:0[1-9]|1[0-2])-(?:0[1-9]|[12][0-9]|3[01])$")
MAX_PACKAGES = 200
MAX_MANIFEST_BYTES = 256 * 1024
MAX_PERMISSIONS = 40
MAX_SCREENSHOTS = 8
PREVIEW_ORIGIN = "https://forkmesh.com/market/previews/"
PREVIEW_RE = re.compile(r"^preview/[A-Za-z0-9_-][A-Za-z0-9._-]{0,80}\.(?:png|jpe?g|webp|svg)$")
SCOPES = ("organization", "account")
PLACES = ("mobile", "desktop", "web")
SETTING_TYPES = ("boolean", "choice")
SETTING_KEY_RE = re.compile(r"^[a-z][A-Za-z0-9]{0,39}$")
SETTING_VALUE_RE = re.compile(r"^[A-Za-z0-9._-]{1,40}$")
MAX_SETTINGS = 16
MONITOR_SOCKET = "status.monitor"
MONITOR_ID_RE = re.compile(r"^[a-z][a-z0-9_]{0,62}$")
MONITOR_PREFIX_RE = re.compile(r"^[a-z][a-z0-9_]{0,30}:$")
MAX_MONITORS = 32
RISKS = ("low", "sensitive", "high")
_CACHE = {}


def _text(value, limit):
    return " ".join(str(value or "").split())[:limit]


def _inside(root, path):
    try:
        path.resolve().relative_to(root.resolve())
    except ValueError:
        return False
    return True


def _read_json(path):
    try:
        raw = path.read_bytes()
    except OSError:
        return None
    if len(raw) > MAX_MANIFEST_BYTES:
        return None
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        return None
    return value if isinstance(value, dict) else None


def _web_route(manifest):
    site = ""
    for item in manifest.get("contributions") or []:
        if not isinstance(item, dict):
            continue
        route = str((item.get("config") or {}).get("route") or "")
        if item.get("socket") == "dashboard.page" and ROUTE_RE.fullmatch(route):
            return route
        if item.get("socket") == "www.page" and SITE_ROUTE_RE.fullmatch(route):
            site = site or route
    return site


def _permissions(manifest):
    rows = []
    for item in (manifest.get("permissions") or [])[:MAX_PERMISSIONS]:
        if not isinstance(item, dict):
            continue
        risk = str(item.get("risk") or "").lower()
        rows.append({
            "id": _text(item.get("id"), 120),
            "label": _text(item.get("label"), 300),
            "risk": risk if risk in RISKS else "sensitive",
        })
    return rows


def _phone_preview_path(path):
    return path[:-4] + ".webp" if path.endswith(".svg") else path


def _screenshots(manifest, root, relative):
    if root is None or not relative:
        return []
    directory = Path(relative).parent
    rows = []
    for item in (manifest.get("screenshots") or [])[:MAX_SCREENSHOTS]:
        if not isinstance(item, dict):
            continue
        path = str(item.get("path") or "")
        if not PREVIEW_RE.fullmatch(path):
            continue
        path = _phone_preview_path(path)
        if not (Path(root) / directory / path).is_file():
            continue
        rows.append({
            "url": PREVIEW_ORIGIN + (directory / path).as_posix(),
            "alt": _text(item.get("alt"), 160),
        })
    return rows


def settings_rows(manifest):
    rows = []
    for item in (manifest.get("settings") or [])[:MAX_SETTINGS]:
        if not isinstance(item, dict):
            continue
        key = str(item.get("key") or "")
        kind = str(item.get("type") or "")
        if not SETTING_KEY_RE.fullmatch(key) or kind not in SETTING_TYPES:
            continue
        row = {"key": key, "type": kind, "label": _text(item.get("label"), 80)}
        if kind == "boolean":
            row["default"] = item.get("default") is True
        else:
            options = [
                {"value": str(option.get("value")), "label": _text(option.get("label"), 60)}
                for option in item.get("options") or []
                if isinstance(option, dict)
                and SETTING_VALUE_RE.fullmatch(str(option.get("value") or ""))
            ]
            values = [option["value"] for option in options]
            if not values:
                continue
            default = str(item.get("default") or "")
            row["default"] = default if default in values else values[0]
            row["options"] = options
        rows.append(row)
    return rows


def _monitors(manifest):
    systems = []
    prefixes = []
    for item in manifest.get("contributions") or []:
        if not isinstance(item, dict) or item.get("socket") != MONITOR_SOCKET:
            continue
        config = item.get("config") if isinstance(item.get("config"), dict) else {}
        for value in config.get("systems") or []:
            value = str(value or "")
            if MONITOR_ID_RE.fullmatch(value) and value not in systems:
                systems.append(value)
        for value in config.get("prefixes") or []:
            value = str(value or "")
            if MONITOR_PREFIX_RE.fullmatch(value) and value not in prefixes:
                prefixes.append(value)
    return {"systems": systems[:MAX_MONITORS], "prefixes": prefixes[:MAX_MONITORS]}


def _dependencies(manifest):
    rows = []
    for item in manifest.get("dependencies") or []:
        value = str((item or {}).get("id") or "") if isinstance(item, dict) else ""
        if ID_RE.fullmatch(value) and value not in rows:
            rows.append(value)
    return rows


def installed_before(manifest):
    install = manifest.get("install")
    value = install.get("installedBefore") if isinstance(install, dict) else None
    return value if isinstance(value, str) and INSTALLED_BEFORE_RE.fullmatch(value) else ""


def places(manifest):
    install = manifest.get("install")
    value = install.get("places") if isinstance(install, dict) else None
    chosen = [place for place in PLACES if isinstance(value, list) and place in value]
    return chosen or list(PLACES)


def package_row(manifest, root=None, relative=""):
    package_id = str(manifest.get("id") or "")
    if not ID_RE.fullmatch(package_id):
        return None
    entrypoints = manifest.get("entrypoints")
    entrypoints = entrypoints if isinstance(entrypoints, dict) else {}
    worker = manifest.get("worker")
    worker = worker if isinstance(worker, dict) else {}
    api = str(worker.get("api") or "")
    scope = str((manifest.get("dataLifecycle") or {}).get("scope") or "")
    version = str(manifest.get("version") or "")
    install = manifest.get("install")
    install = install if isinstance(install, dict) else {}
    price = manifest.get("price")
    price = price if isinstance(price, dict) else {}
    row = {
        "id": package_id,
        "market": MARKET,
        "name": _text(manifest.get("name"), 80) or package_id,
        "version": version if VERSION_RE.fullmatch(version) else "",
        "summary": _text(manifest.get("summary"), 600),
        "category": _text(manifest.get("category"), 40),
        "type": _text(manifest.get("type"), 40),
        "surfaces": {
            "desktop": "desktop" in entrypoints,
            "web": _web_route(manifest),
            "worker": bool(API_RE.fullmatch(api)),
            "world": "world" in entrypoints,
            "sdk": "sdk" in entrypoints,
        },
        "api": api if API_RE.fullmatch(api) else "",
        "dataScope": scope if scope in SCOPES else "",
        "bundled": bool(install.get("bundled")),
        "default": install.get("default") is True,
        "accountSync": install.get("sync") == "account",
        "places": places(manifest),
        "dependencies": _dependencies(manifest),
        "settings": settings_rows(manifest),
        "monitors": _monitors(manifest),
        "free": not int(price.get("amount") or 0),
        "permissions": _permissions(manifest),
        "screenshots": _screenshots(manifest, root, relative),
    }
    if installed_before(manifest):
        row["installedBefore"] = installed_before(manifest)
    return row


def catalog_packages(root=ROOT):
    key = str(root)
    if key in _CACHE:
        return _CACHE[key]
    catalog = _read_json(Path(root) / "catalog.json") or {}
    rows = []
    seen = set()
    for entry in (catalog.get("packages") or [])[:MAX_PACKAGES]:
        if not isinstance(entry, dict):
            continue
        relative = str(entry.get("manifest") or "")
        if not relative.endswith("manifest.json") or ".." in relative:
            continue
        path = Path(root) / relative
        if not _inside(Path(root), path):
            continue
        manifest = _read_json(path)
        row = package_row(manifest, root, relative) if manifest else None
        if not row or row["id"] in seen:
            continue
        seen.add(row["id"])
        rows.append(row)
    rows.sort(key=lambda row: row["name"].lower())
    _CACHE[key] = rows
    return rows


def staged_packages():
    try:
        from plugin_registry import MARKET_PACKAGES
    except ImportError:
        return []
    return [dict(row) for row in MARKET_PACKAGES]


def payload(root=ROOT):
    packages = staged_packages() if root == ROOT else []
    if not packages:
        packages = catalog_packages(root)
    return {
        "ok": True,
        "schemaVersion": 1,
        "market": MARKET,
        "count": len(packages),
        "packages": packages,
    }


async def handle(method, json_response, root=ROOT):
    if method not in ("GET", "HEAD"):
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control="no-store", extra_headers={"allow": "GET"})
    return json_response(
        payload(root), cache_control="public, max-age=300",
        extra_headers={"x-content-type-options": "nosniff"})

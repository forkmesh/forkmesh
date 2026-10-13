#!/usr/bin/env python3
"""Stage Marketplace contributions into generic App sockets.

Nothing generated here is written to ``app/public`` or ``app/src``. Web
contributions are assembled into the ignored ``app/dist`` tree and Worker
contributions into ignored ``app/src_build``. A plugin PR therefore owns only
its manifest and package content; the application remains a package-neutral
host.
"""

from __future__ import annotations

import hashlib
import html
import json
from pathlib import Path
import os
import re
import sys
import zipfile


APP = Path(__file__).resolve().parents[1]
REPOSITORY = APP.parent
MARKETPLACE_TOOLS = REPOSITORY / "marketplace" / "tools"
APP_SOURCE = APP / "src"
for import_root in (MARKETPLACE_TOOLS, APP_SOURCE):
    if str(import_root) not in sys.path:
        sys.path.insert(0, str(import_root))

from marketplace_lib import (  # noqa: E402
    MarketplaceError,
    load_catalog,
    manifest_entries,
)
from plugin_sockets import Contribution, discover  # noqa: E402
import dashboard_shell  # noqa: E402
import market_packages  # noqa: E402
import workflow_files  # noqa: E402


CATALOG = REPOSITORY / "marketplace" / "catalog.json"
WEB_SOCKET = "dashboard.page"
HTTP_SOCKET = "worker.http"
PLUGIN_NAV_MARKER = dashboard_shell.PLUGIN_NAV_MARKER
ROUTE_RE = re.compile(r"^/dashboard/[a-z0-9][a-z0-9./-]*$")
ICON_RE = re.compile(r"^[a-z][a-z0-9-]{0,63}$")
HANDLER_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
METHODS = frozenset({"DELETE", "GET", "HEAD", "PATCH", "POST", "PUT"})
EXTENSION_VERSION_RE = re.compile(r"^[0-9]{1,5}(\.[0-9]{1,5}){0,3}$")
WWW_SOCKET = "www.page"
CONTENT_SOCKETS = {
    "content.action": "action",
    "content.shortcut": "shortcut",
    "content.skill": "skill",
    "content.teammate": "agent",
}
MAX_CONTENT_FILES = 32
WWW_ROUTE_RE = re.compile(r"^/[a-z][a-z0-9-]{0,39}$")
WWW_ASSET_SUFFIXES = {".css", ".js", ".json", ".svg", ".png", ".webp"}


def _key(item: Contribution) -> str:
    return (
        item.package_id if item.contribution_id == "default"
        else item.package_id + "." + item.contribution_id
    )


def _excluded() -> set[str]:
    raw = os.environ.get("FORKMESH_EXCLUDED_PLUGINS", "")
    return {item.strip() for item in raw.split(",") if item.strip()}


def _active(catalog_path: Path, socket: str):
    """Yield bundled contributions enabled for this App deployment."""
    excluded = _excluded()
    for item in discover(catalog_path, {socket}):
        install = item.manifest.get("install") or {}
        if install.get("bundled") is True and item.package_id not in excluded:
            yield item


def _web_entries(catalog_path: Path = CATALOG) -> list[dict]:
    entries = []
    routes = {
        dashboard_shell.PAGES[page]["route"] for page in dashboard_shell.PAGES
    }
    for item in _active(catalog_path, WEB_SOCKET):
        config = item.config
        allowed = {"route", "title", "description", "navigation", "script"}
        if set(config) - allowed:
            raise MarketplaceError(
                f"package {item.package_id} dashboard.page has unsupported config"
            )
        route = str(config.get("route") or "")
        if not ROUTE_RE.fullmatch(route) or route in routes:
            raise MarketplaceError(
                f"package {item.package_id} dashboard.page route is invalid or duplicated"
            )
        routes.add(route)
        if item.entrypoint.suffix.lower() != ".html":
            raise MarketplaceError(
                f"package {item.package_id} dashboard.page entrypoint must be HTML"
            )
        title = str(config.get("title") or item.manifest["name"])
        description = str(config.get("description") or item.manifest["summary"])
        if not title or len(title) > 120 or not description or len(description) > 240:
            raise MarketplaceError(
                f"package {item.package_id} dashboard.page metadata is invalid"
            )
        navigation = config.get("navigation") or {}
        if (
            not isinstance(navigation, dict)
            or set(navigation) - {"label", "icon", "order"}
        ):
            raise MarketplaceError(
                f"package {item.package_id} dashboard.page navigation is invalid"
            )
        label = str(navigation.get("label") or item.manifest["name"])
        icon = str(navigation.get("icon") or "plug")
        order = navigation.get("order", 100)
        if (
            not label or len(label) > 80 or not ICON_RE.fullmatch(icon)
            or isinstance(order, bool) or not isinstance(order, int)
            or not -10000 <= order <= 10000
        ):
            raise MarketplaceError(
                f"package {item.package_id} dashboard.page navigation is invalid"
            )
        script_path = None
        script_name = str(config.get("script") or "")
        manifest_root = _package_root(item)
        if script_name:
            raw_path = str(item.manifest["entrypoints"].get(script_name) or "")
            script_path = (manifest_root / raw_path).resolve()
            try:
                script_path.relative_to(manifest_root.resolve())
            except ValueError as error:
                raise MarketplaceError(
                    f"package {item.package_id} dashboard script escapes its package"
                ) from error
            if (
                not raw_path.startswith("content/")
                or not script_path.is_file()
                or script_path.suffix != ".js"
            ):
                raise MarketplaceError(
                    f"package {item.package_id} dashboard script entrypoint is invalid"
                )
        extension_asset, extension_dir = _browser_extension(item, manifest_root)
        key = _key(item)
        entries.append({
            "key": key,
            "item": item,
            "route": route,
            "asset": "dashboard-plugins/%s/%s/index.html" % (
                item.package_id, item.contribution_id),
            "script_asset": (
                "dashboard-plugins/%s/%s/page.js" % (
                    item.package_id, item.contribution_id)
                if script_path else ""
            ),
            "script_path": script_path,
            "extension_asset": extension_asset,
            "extension_dir": extension_dir,
            "title": title,
            "description": description,
            "label": label,
            "icon": icon,
            "order": order,
        })
    return sorted(
        entries,
        key=lambda row: (row["order"], row["label"].lower(), row["key"]),
    )


def _navigation(entries: list[dict]) -> str:
    if not entries:
        return ""
    links = []
    for entry in entries:
        links.append(
            '            <a data-nav="{key}" data-nav-link '
            'data-plugin-nav="{plugin}" href="{route}" class="hidden">'
            '<span class="dashboard-nav-icon"><i data-lucide="{icon}"></i>'
            '</span>{label}</a>'.format(
                key=html.escape(entry["key"], quote=True),
                plugin=html.escape(entry["item"].package_id, quote=True),
                route=html.escape(entry["route"], quote=True),
                icon=html.escape(entry["icon"], quote=True),
                label=html.escape(entry["label"]),
            )
        )
    return (
        '          <div role="group" aria-labelledby="sidebarGroupPlugins" '
        'data-plugin-navigation class="dashboard-nav-group hidden">\n'
        '            <span id="sidebarGroupPlugins" '
        'class="dashboard-nav-title">Plugins</span>\n'
        + "\n".join(links) + "\n          </div>"
    )


def _package_root(item: Contribution) -> Path:
    """The package directory, resolved from the manifest's ``content`` tree
    independent of how deep the contribution's entrypoint sits."""
    root = item.entrypoint
    while root.name != "content" and root != root.parent:
        root = root.parent
    return root.parent


def _extension_stem(package_id: str) -> str:
    """The zip's base name and its one top-level folder. A package that is
    itself the extension keeps its own name rather than repeating the word."""
    return package_id if package_id.endswith("-extension") else package_id + "-extension"


def _browser_extension(item: Contribution, package_root: Path):
    """The zip a dashboard page offers for its package's browser extension:
    ``(asset, folder)``, or ``("", None)`` when the package ships none. The
    version in the name is the extension manifest's, so a page can link the
    exact build it expects and a new release never hits a cached zip."""
    raw_path = str(item.manifest["entrypoints"].get("browser-extension") or "")
    if not raw_path:
        return "", None
    manifest_path = (package_root / raw_path).resolve()
    try:
        manifest_path.relative_to(package_root.resolve())
    except ValueError as error:
        raise MarketplaceError(
            f"package {item.package_id} browser extension escapes its package"
        ) from error
    if (
        not raw_path.startswith("content/")
        or manifest_path.name != "manifest.json"
        or not manifest_path.is_file()
    ):
        raise MarketplaceError(
            f"package {item.package_id} browser extension entrypoint is invalid"
        )
    try:
        version = str(json.loads(manifest_path.read_text(encoding="utf-8"))["version"])
    except (ValueError, KeyError, TypeError) as error:
        raise MarketplaceError(
            f"package {item.package_id} browser extension manifest is invalid"
        ) from error
    if not EXTENSION_VERSION_RE.fullmatch(version):
        raise MarketplaceError(
            f"package {item.package_id} browser extension version is invalid"
        )
    return (
        "dashboard-plugins/%s/%s-%s.zip" % (
            item.package_id, _extension_stem(item.package_id), version),
        manifest_path.parent,
    )


def _write_extension_zip(target: Path, folder: Path, prefix: str) -> None:
    """Zip ``folder`` under one top-level ``prefix`` directory, byte-for-byte
    reproducible: sorted names and a fixed timestamp. Unzipping a newer
    release over the same place keeps an unpacked extension's identity."""
    target.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(folder.rglob("*")):
            if not path.is_file():
                continue
            info = zipfile.ZipInfo(
                "%s/%s" % (prefix, path.relative_to(folder).as_posix()),
                date_time=(1980, 1, 1, 0, 0, 0),
            )
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, path.read_bytes())


def stage_web(destination: Path, catalog_path: Path = CATALOG) -> dict[str, str]:
    """Fill dashboard page/navigation sockets in a deployment asset tree."""
    entries = _web_entries(catalog_path)
    navigation = _navigation(entries)
    for meta in dashboard_shell.PAGES.values():
        path = destination / meta["asset"]
        if not path.is_file():
            continue
        source = path.read_text(encoding="utf-8")
        if PLUGIN_NAV_MARKER not in source:
            raise MarketplaceError(
                f"dashboard asset {meta['asset']} has no plugin socket"
            )
        path.write_text(
            source.replace(PLUGIN_NAV_MARKER, navigation), encoding="utf-8"
        )

    public = APP / "public"
    read = lambda relative: (public / relative).read_text(encoding="utf-8")
    pages = {}
    for entry in entries:
        script_tag = (
            '<script src="/%s" defer></script>' % entry["script_asset"]
            if entry["script_asset"] else ""
        )
        meta = {
            "view": "",
            "section": entry["key"],
            "nav": entry["key"],
            "route": entry["route"],
            "asset": entry["asset"],
            "title": html.escape(entry["title"]) + " - ForkMesh",
            "description": html.escape(entry["description"], quote=True),
        }
        document = dashboard_shell.compose_page_from_reader(
            read,
            entry["key"],
            page=meta,
            view=entry["item"].entrypoint.read_text(encoding="utf-8"),
            plugin_script=script_tag,
        ).replace(PLUGIN_NAV_MARKER, navigation)
        # Core pages are marked active while their authored nav is already in
        # the shell. Plugin navigation is injected only above, so apply the
        # same server-rendered active state after the socket is filled.
        document = dashboard_shell.mark_active_nav(
            document, entry["key"], page=meta
        )
        target = destination / entry["asset"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(document, encoding="utf-8")
        if entry["script_path"]:
            script_target = destination / entry["script_asset"]
            script_target.parent.mkdir(parents=True, exist_ok=True)
            script_target.write_bytes(entry["script_path"].read_bytes())
        if entry["extension_asset"]:
            _write_extension_zip(
                destination / entry["extension_asset"],
                entry["extension_dir"],
                _extension_stem(entry["item"].package_id),
            )
        pages[entry["route"]] = entry["asset"]
    registry = destination / "dashboard-plugins" / "registry.json"
    registry.parent.mkdir(parents=True, exist_ok=True)
    registry.write_text(
        json.dumps({"pages": pages}, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return pages


def _www_entrypoint(item: Contribution, name: str, suffixes: set[str]) -> Path:
    package_root = _package_root(item)
    raw_path = str(item.manifest["entrypoints"].get(name) or "")
    path = (package_root / raw_path).resolve()
    try:
        path.relative_to(package_root.resolve())
    except ValueError as error:
        raise MarketplaceError(
            f"package {item.package_id} www.page entrypoint escapes its package"
        ) from error
    if (
        not raw_path.startswith("content/")
        or not path.is_file()
        or path.suffix.lower() not in suffixes
    ):
        raise MarketplaceError(
            f"package {item.package_id} www.page entrypoint {name} is invalid"
        )
    return path


def www_entries(catalog_path: Path = CATALOG) -> list[dict]:
    import static_routes

    reserved = set(static_routes.CORE_RESERVED_ROUTE_PREFIXES)
    taken = {path.stem for path in (REPOSITORY / "www" / "public").glob("*.html")}
    taken |= {path.name for path in (REPOSITORY / "www" / "public").iterdir() if path.is_dir()}
    entries = []
    for item in _active(catalog_path, WWW_SOCKET):
        config = item.config
        if set(config) - {"route", "title", "assets", "builder"}:
            raise MarketplaceError(
                f"package {item.package_id} www.page has unsupported config"
            )
        route = str(config.get("route") or "")
        name = route[1:]
        if not WWW_ROUTE_RE.fullmatch(route) or name in reserved or name in taken:
            raise MarketplaceError(
                f"package {item.package_id} www.page route is invalid or taken"
            )
        taken.add(name)
        if item.entrypoint.suffix.lower() != ".html":
            raise MarketplaceError(
                f"package {item.package_id} www.page entrypoint must be HTML"
            )
        assets = config.get("assets") or []
        if not isinstance(assets, list) or len(assets) > 16:
            raise MarketplaceError(
                f"package {item.package_id} www.page assets are invalid"
            )
        builder = str(config.get("builder") or "")
        entries.append({
            "item": item,
            "route": route,
            "name": name,
            "page": item.entrypoint,
            "assets": [
                _www_entrypoint(item, str(asset), WWW_ASSET_SUFFIXES)
                for asset in assets
            ],
            "builder": _www_entrypoint(item, builder, {".py"}) if builder else None,
        })
    return entries


def www_routes(catalog_path: Path = CATALOG) -> tuple[str, ...]:
    return tuple(sorted(entry["name"] for entry in www_entries(catalog_path)))


def _run_www_builder(entry: dict, destination: Path) -> None:
    import importlib.util

    module_name = "market_www_builder_%s" % re.sub(
        r"[^A-Za-z0-9_]", "_", entry["item"].package_id)
    spec = importlib.util.spec_from_file_location(module_name, entry["builder"])
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    build = getattr(module, "build", None)
    if not callable(build):
        raise MarketplaceError(
            f"package {entry['item'].package_id} www.page builder has no build()"
        )
    build(REPOSITORY, destination)


def stage_www_pages(destination: Path, catalog_path: Path = CATALOG) -> list[str]:
    staged = []
    for entry in www_entries(catalog_path):
        page = destination / (entry["name"] + ".html")
        folder = destination / entry["name"]
        if page.exists() or folder.exists():
            raise MarketplaceError(
                f"package {entry['item'].package_id} www.page {entry['route']} is already staged"
            )
        page.write_bytes(entry["page"].read_bytes())
        folder.mkdir(parents=True)
        for asset in entry["assets"]:
            (folder / asset.name).write_bytes(asset.read_bytes())
        if entry["builder"]:
            _run_www_builder(entry, folder)
        _stamp_www_versions(entry["name"], page, folder)
        staged.append(entry["route"])
    return staged


def _www_versions(name: str, folder: Path) -> dict[str, str]:
    versions = {}
    for path in sorted(folder.rglob("*")):
        if path.is_file():
            relative = path.relative_to(folder).as_posix()
            versions["/%s/%s?v=${version}" % (name, relative)] = "/%s/%s?v=%s" % (
                name, relative, hashlib.sha256(path.read_bytes()).hexdigest()[:12])
    return versions


def _replace_versions(target: Path, versions: dict[str, str]) -> None:
    source = target.read_text(encoding="utf-8")
    stamped = source
    for placeholder, value in versions.items():
        stamped = stamped.replace(placeholder, value)
    if stamped != source:
        target.write_text(stamped, encoding="utf-8")


def _stamp_www_versions(name: str, page: Path, folder: Path) -> None:
    scripts = sorted(path for path in folder.iterdir()
                     if path.is_file() and path.suffix in {".css", ".js"})
    for script in scripts:
        _replace_versions(script, _www_versions(name, folder))
    _replace_versions(page, _www_versions(name, folder))


def _worker_entries(catalog_path: Path = CATALOG) -> list[dict]:
    routes = set()
    entries = []
    for item in _active(catalog_path, HTTP_SOCKET):
        config = item.config
        if set(config) - {"route", "handler", "methods"}:
            raise MarketplaceError(
                f"package {item.package_id} worker.http has unsupported config"
            )
        route = str(config.get("route") or "").rstrip("/")
        namespace = "/api/plugins/" + item.package_id
        if not route or (route != namespace and not route.startswith(namespace + "/")):
            raise MarketplaceError(
                f"package {item.package_id} worker.http must use its {namespace} namespace"
            )
        if route in routes:
            raise MarketplaceError(f"duplicate plugin HTTP route {route}")
        routes.add(route)
        handler = str(config.get("handler") or "handle")
        methods = config.get("methods") or ["GET"]
        if (
            item.entrypoint.suffix != ".py" or not HANDLER_RE.fullmatch(handler)
            or not isinstance(methods, list) or not methods
        ):
            raise MarketplaceError(
                f"package {item.package_id} worker.http config is invalid"
            )
        normalized_methods = tuple(sorted({str(method).upper() for method in methods}))
        if not set(normalized_methods).issubset(METHODS):
            raise MarketplaceError(
                f"package {item.package_id} worker.http methods are invalid"
            )
        identity = item.package_id + ":" + item.contribution_id
        module = "market_plugin_%s_%s" % (
            re.sub(r"[^A-Za-z0-9_]", "_", identity),
            hashlib.sha256(identity.encode("utf-8")).hexdigest()[:10],
        )
        entries.append({
            "item": item, "route": route, "handler": handler,
            "methods": normalized_methods, "module": module,
        })
    return sorted(entries, key=lambda row: (-len(row["route"]), row["route"]))


def content_files(manifest: dict, package_root: Path) -> tuple:
    """Repository files a package offers (``content.*`` sockets), with text."""
    files = []
    for item in manifest.get("contributions") or ():
        kind = CONTENT_SOCKETS.get(str((item or {}).get("socket") or ""))
        if not kind:
            continue
        config = item.get("config") if isinstance(item.get("config"), dict) else {}
        target = str(config.get("path") or "")
        name = str(config.get("name") or "")
        raw_path = str(manifest["entrypoints"].get(item.get("entrypoint")) or "")
        source = (package_root / raw_path).resolve()
        try:
            source.relative_to(package_root.resolve())
        except ValueError as error:
            raise MarketplaceError(
                f"package {manifest['id']} content file escapes its package"
            ) from error
        if (
            not raw_path.startswith("content/") or not source.is_file()
            or workflow_files.kind_for_path(target) != kind
            or not name or len(name) > 80
        ):
            raise MarketplaceError(
                f"package {manifest['id']} {item.get('socket')} contribution is invalid"
            )
        text = source.read_text(encoding="utf-8")
        if len(text.encode("utf-8")) > workflow_files.MAX_FILE_BYTES:
            raise MarketplaceError(
                f"package {manifest['id']} content file is too large"
            )
        files.append({"kind": kind, "path": target, "name": name, "text": text})
    if len(files) > MAX_CONTENT_FILES:
        raise MarketplaceError(f"package {manifest['id']} ships too many files")
    return tuple(files)


def registry_source(catalog_path: Path = CATALOG) -> str:
    pages = {row["route"]: row["asset"] for row in _web_entries(catalog_path)}
    html_paths = sorted("/" + asset for asset in pages.values())
    routes = tuple(
        (row["route"], row["methods"], row["module"], row["handler"])
        for row in _worker_entries(catalog_path)
    )
    plugins = {}
    packages = []
    catalog = load_catalog(catalog_path)
    for entry, manifest in manifest_entries(catalog):
        row = market_packages.package_row(manifest, catalog.root, entry.manifest)
        if row and all(row["id"] != item["id"] for item in packages):
            packages.append(row)
        worker = manifest.get("worker") or {}
        lifecycle = (manifest.get("dataLifecycle") or {}).get("scope") or "none"
        plugins[manifest["id"]] = {
            "module": str(worker.get("module") or ""),
            "api": str(worker.get("api") or ""),
            "outboundOrigins": tuple(worker.get("outboundOrigins") or ()),
            "permissions": tuple(str(item.get("id") or "") for item in
                                 manifest.get("permissions") or ()),
            "organizationData": lifecycle == "organization",
            "accountData": lifecycle == "account",
            "default": (manifest.get("install") or {}).get("default") is True,
            "accountSync": (manifest.get("install") or {}).get("sync") == "account",
            "installedBefore": market_packages.installed_before(manifest),
            "places": tuple(market_packages.places(manifest)),
            "dependencies": tuple(
                str(item.get("id") or "") for item in manifest.get("dependencies") or ()),
            "settings": tuple(market_packages.settings_rows(manifest)),
            "files": content_files(
                manifest, (catalog.root / entry.manifest).parent),
        }
    return '''"""Generated in app/src_build from Marketplace socket contributions."""

PLUGIN_DASHBOARD_PAGE_ASSETS = %r
PLUGIN_STATIC_HTML_PATHS = frozenset(%r)
PLUGIN_WWW_SITE_ROUTES = frozenset(%r)
HTTP_ROUTES = %r
PLUGINS = %r
MARKET_PACKAGES = %r


def api_prefixes():
    """Every package Worker API prefix, for host request placement."""
    return tuple(sorted(
        str(plugin.get("api") or "")
        for plugin in PLUGINS.values() if plugin.get("api")))


async def dispatch_http(request, env, path, method):
    for prefix, methods, module_name, handler_name in HTTP_ROUTES:
        if (path == prefix or path.startswith(prefix + "/")) and method in methods:
            module = __import__(module_name)
            return await getattr(module, handler_name)(request, env)
    return None
''' % (pages, html_paths, www_routes(catalog_path), routes, plugins, tuple(
        sorted(packages, key=lambda row: row["name"].lower())
    )[:market_packages.MAX_PACKAGES])


def worker_payloads(catalog_path: Path = CATALOG) -> list[tuple[Path, int, bytes]]:
    """Return generated registry/package modules for ``src_build``."""
    payloads = []
    registry = registry_source(catalog_path).encode("utf-8")
    payloads.append((Path("plugin_registry.py"), len(registry), registry))
    # Package lifecycle/API modules are declared in the manifest's worker
    # descriptor, independently from the lower-level worker.http socket. CRM
    # uses this route so the host can provide sessions, D1 and encryption while
    # the package owns every API rule and schema hook. Stage those modules too;
    # previously the generated registry named them but the deploy omitted the
    # files, making the first production request fail at import time.
    catalog = load_catalog(catalog_path)
    staged_modules = set()
    for entry, manifest in manifest_entries(catalog):
        worker = manifest.get("worker") or {}
        module = str(worker.get("module") or "")
        api = str(worker.get("api") or "")
        if not module and not api:
            continue
        if (
            not module or not api or not HANDLER_RE.fullmatch(module)
            or not api.startswith("/api/")
        ):
            raise MarketplaceError(
                f"package {manifest['id']} worker descriptor is invalid"
            )
        raw_path = str((manifest.get("entrypoints") or {}).get("worker") or "")
        package_root = (catalog.root / entry.manifest).parent.resolve()
        source = (package_root / raw_path).resolve()
        try:
            source.relative_to(package_root)
        except ValueError as error:
            raise MarketplaceError(
                f"package {manifest['id']} worker entrypoint escapes its package"
            ) from error
        if (
            not raw_path.startswith("content/worker/")
            or source.suffix != ".py" or not source.is_file()
            or source.stem != module or module in staged_modules
        ):
            raise MarketplaceError(
                f"package {manifest['id']} worker entrypoint is invalid"
            )
        staged_modules.add(module)
        raw = source.read_bytes()
        payloads.append((Path(module + ".py"), len(raw), raw))
    for row in _worker_entries(catalog_path):
        if row["module"] in staged_modules:
            raise MarketplaceError("duplicate staged Worker plugin module")
        raw = row["item"].entrypoint.read_bytes()
        payloads.append((Path(row["module"] + ".py"), len(raw), raw))
    return payloads

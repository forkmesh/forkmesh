#!/usr/bin/env python3
"""Marketplace packages attach to App sockets without changing App source."""

import json
import sys
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
APP = REPOSITORY / "app"
for root in (REPOSITORY / "marketplace" / "tools", APP / "tools"):
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))

from marketplace_lib import load_manifest  # noqa: E402
from plugin_sockets import discover  # noqa: E402
import build_app_plugins  # noqa: E402


def _fixture(root: Path) -> Path:
    package = root / "packages" / "demo-plugin"
    (package / "content" / "web").mkdir(parents=True)
    (package / "content" / "worker").mkdir(parents=True)
    (package / "preview").mkdir()
    (package / "preview" / "market.png").write_bytes(b"png")
    (package / "content" / "web" / "view.html").write_text(
        '<section data-view="demo-plugin.page" class="view active">Demo socket</section>\n',
        encoding="utf-8",
    )
    (package / "content" / "web" / "page.js").write_text(
        'window.ForkMeshDashboard.registerPage("demo-plugin.page", ({ select }) => '
        'select("[data-view]")?.setAttribute("data-ready", "true"));\n',
        encoding="utf-8",
    )
    (package / "content" / "worker" / "api.py").write_text(
        "async def serve(request, env):\n"
        "    return {'request': request, 'env': env}\n",
        encoding="utf-8",
    )
    manifest = {
        "schemaVersion": 1,
        "id": "demo-plugin",
        "name": "Demo Plugin",
        "creator": {
            "id": "did:forkmesh:tester", "handle": "tester",
            "displayName": "Tester", "verified": False,
        },
        "type": "extension.native",
        "version": "1.2.3",
        "summary": "Proves that packages attach only through generic sockets.",
        "license": {
            "model": "free-open-source", "spdx": "MIT",
            "creatorRetainsOwnership": True,
        },
        "price": {"amount": 0, "currency": "USD"},
        "entrypoints": {
            "settings": "builtin:package",
            "dashboard-view": "content/web/view.html",
            "dashboard-script": "content/web/page.js",
            "worker": "content/worker/api.py",
        },
        "contributions": [
            {
                "id": "page",
                "socket": "dashboard.page",
                "entrypoint": "dashboard-view",
                "config": {
                    "route": "/dashboard/demo",
                    "title": "Demo",
                    "script": "dashboard-script",
                    "navigation": {"label": "Demo", "icon": "plug", "order": 5},
                },
            },
            {
                "id": "api",
                "socket": "worker.http",
                "entrypoint": "worker",
                "config": {
                    "route": "/api/plugins/demo-plugin/records",
                    "handler": "serve",
                    "methods": ["GET", "POST"],
                },
            },
            {
                "id": "future",
                "socket": "future.host-slot",
                "entrypoint": "dashboard-view",
                "config": {"hostOwnsThisContract": True},
            },
        ],
        "permissions": [],
        "screenshots": [{"path": "preview/market.png", "alt": "Demo"}],
        "dependencies": [],
        "trust": {"signature": "verified-bundle"},
        "install": {"bundled": True, "default": True},
        "lifecycle": {"migrations": []},
    }
    (package / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    catalog = {
        "schemaVersion": 2,
        "marketplace": {
            "id": "test", "name": "Test",
            "locations": [{"transport": "local", "url": ".", "catalog": "catalog.json"}],
        },
        "packages": [{
            "id": "demo-plugin", "version": "1.2.3",
            "manifest": "packages/demo-plugin/manifest.json",
        }],
    }
    catalog_path = root / "catalog.json"
    catalog_path.write_text(json.dumps(catalog), encoding="utf-8")
    (root / "plugins.json").write_text(
        json.dumps({"schemaVersion": 1, "plugins": ["demo-plugin"]}), encoding="utf-8")
    return catalog_path


def test_manifest_accepts_host_neutral_socket_names(tmp_path):
    catalog = _fixture(tmp_path)
    manifest = load_manifest(tmp_path / "packages/demo-plugin/manifest.json")
    assert len(manifest["contributions"]) == 3
    assert [item.socket for item in discover(catalog)] == [
        "dashboard.page", "worker.http", "future.host-slot",
    ]


def test_dashboard_package_is_written_only_to_deploy_stage(tmp_path):
    catalog = _fixture(tmp_path / "market")
    destination = tmp_path / "dist"
    base_page = destination / "dashboard" / "index.html"
    base_page.parent.mkdir(parents=True)
    base_page.write_text(build_app_plugins.PLUGIN_NAV_MARKER, encoding="utf-8")

    pages = build_app_plugins.stage_web(destination, catalog)

    assert pages == {
        "/dashboard/demo": "dashboard-plugins/demo-plugin/page/index.html"
    }
    assert "Demo" in base_page.read_text(encoding="utf-8")
    plugin_page = destination / pages["/dashboard/demo"]
    plugin_html = plugin_page.read_text(encoding="utf-8")
    assert "Demo socket" in plugin_html
    assert plugin_html.index("/dashboard-plugin-host.js") < plugin_html.index(
        "/dashboard-plugins/demo-plugin/page/page.js"
    ) < plugin_html.index("/dashboard.js")
    assert (destination / "dashboard-plugins/demo-plugin/page/page.js").is_file()
    assert 'data-page="demo-plugin.page" data-plugin-package="demo-plugin" data-plugin-name="' in plugin_html
    assert "data-plugin-menu-toggle" in plugin_html
    assert not (APP / "public" / "dashboard-plugins").exists()
    assert "demo-plugin" not in (APP / "src" / "plugin_registry.py").read_text(
        encoding="utf-8"
    )


def test_browser_extension_is_zipped_by_version_into_the_deploy_stage(tmp_path):
    import zipfile

    catalog = _fixture(tmp_path / "market")
    package = tmp_path / "market" / "packages" / "demo-plugin"
    extension = package / "content" / "extension"
    (extension / "icons").mkdir(parents=True)
    (extension / "manifest.json").write_text('{"version": "2.0.1"}', encoding="utf-8")
    (extension / "icons" / "icon16.png").write_bytes(b"png")
    manifest_path = package / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["entrypoints"]["browser-extension"] = "content/extension/manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    first, second = tmp_path / "dist-a", tmp_path / "dist-b"

    build_app_plugins.stage_web(first, catalog)
    build_app_plugins.stage_web(second, catalog)

    relative = "dashboard-plugins/demo-plugin/demo-plugin-extension-2.0.1.zip"
    with zipfile.ZipFile(first / relative) as archive:
        assert archive.namelist() == [
            "demo-plugin-extension/icons/icon16.png",
            "demo-plugin-extension/manifest.json",
        ]
    # Reproducible: the same package stages the same bytes.
    assert (first / relative).read_bytes() == (second / relative).read_bytes()


def test_a_package_that_is_the_extension_keeps_its_own_zip_name():
    # A package contributing an extension is zipped as <id>-extension; the
    # package that *is* the extension does not say the word twice.
    assert build_app_plugins._extension_stem("crm") == "crm-extension"
    assert build_app_plugins._extension_stem("browser-extension") == "browser-extension"
    entries = {row["key"]: row for row in build_app_plugins._web_entries()}
    version = json.loads((
        REPOSITORY / "marketplace/packages/browser-extension/content/extension/manifest.json"
    ).read_text(encoding="utf-8"))["version"]
    assert entries["browser-extension"]["extension_asset"] == (
        "dashboard-plugins/browser-extension/browser-extension-%s.zip" % version)
    # The CRM no longer ships one: the extension is its own Market package.
    assert entries["crm"]["extension_asset"] == ""


def test_worker_registry_and_module_are_generated_only_for_src_build(tmp_path):
    catalog = _fixture(tmp_path)
    manifest_path = tmp_path / "packages/demo-plugin/manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["worker"] = {
        "module": "api",
        "api": "/api/demo-plugin",
        "outboundOrigins": ["https://api.example.test"],
    }
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    payloads = {
        path.as_posix(): payload
        for path, _raw_size, payload in build_app_plugins.worker_payloads(catalog)
    }

    registry = payloads["plugin_registry.py"].decode("utf-8")
    assert "/dashboard/demo" in registry
    assert "/api/plugins/demo-plugin/records" in registry
    namespace = {}
    exec(registry, namespace)
    assert namespace["PLUGINS"]["demo-plugin"]["outboundOrigins"] == (
        "https://api.example.test",
    )
    # The stateless dispatcher places package API requests on their own
    # ingress object, and this is the only place it may learn their paths.
    assert namespace["api_prefixes"]() == ("/api/demo-plugin",)
    checked_in = {}
    exec((APP / "src" / "plugin_registry.py").read_text(encoding="utf-8"),
         checked_in)
    assert checked_in["api_prefixes"]() == ()
    assert any(name.startswith("plugins/market_plugin_demo_plugin_api_")
               for name in payloads)
    assert payloads["plugins/api.py"]
    assert not any(path.name.startswith("market_plugin_demo_plugin")
                   for path in (APP / "src").glob("*.py"))


def test_catalog_publication_does_not_activate_an_opt_in_package(tmp_path):
    catalog = _fixture(tmp_path)
    manifest_path = tmp_path / "packages/demo-plugin/manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["install"]["bundled"] = False
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

    assert discover(catalog)
    namespace = {}
    exec(build_app_plugins.registry_source(catalog), namespace)
    assert namespace["PLUGIN_DASHBOARD_PAGE_ASSETS"] == {}
    assert namespace["HTTP_ROUTES"] == ()
    assert [path.as_posix() for path, *_ in
            build_app_plugins.worker_payloads(catalog)] == ["plugin_registry.py"]


def _with_worker(catalog):
    manifest_path = catalog.parent / "packages/demo-plugin/manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["worker"] = {"module": "api", "api": "/api/demo-plugin"}
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return catalog


def _staged(catalog):
    namespace = {}
    exec(build_app_plugins.registry_source(catalog), namespace)
    return namespace, [
        path.as_posix() for path, *_ in build_app_plugins.worker_payloads(catalog)]


def test_a_plugin_left_out_of_plugins_json_is_not_part_of_the_app(tmp_path):
    catalog = _with_worker(_fixture(tmp_path))
    (tmp_path / "plugins.json").write_text(
        json.dumps({"schemaVersion": 1, "plugins": []}), encoding="utf-8")

    namespace, staged = _staged(catalog)

    assert staged == ["plugin_registry.py"]
    assert namespace["PLUGINS"] == {}
    assert namespace["MARKET_PACKAGES"] == ()
    assert namespace["PLUGIN_DASHBOARD_PAGE_ASSETS"] == {}
    assert namespace["HTTP_ROUTES"] == ()


def test_the_environment_adds_or_removes_plugins(tmp_path, monkeypatch):
    catalog = _with_worker(_fixture(tmp_path))

    monkeypatch.setenv("FORKMESH_PLUGINS", "none")
    assert _staged(catalog)[1] == ["plugin_registry.py"]

    monkeypatch.setenv("FORKMESH_PLUGINS", "demo-plugin")
    namespace, staged = _staged(catalog)
    assert "plugins/api.py" in staged
    assert set(namespace["PLUGINS"]) == {"demo-plugin"}

    monkeypatch.setenv("FORKMESH_PLUGINS", "default")
    monkeypatch.setenv("FORKMESH_EXCLUDED_PLUGINS", "demo-plugin")
    namespace, staged = _staged(catalog)
    assert staged == ["plugin_registry.py"]
    assert namespace["PLUGINS"] == {}


def test_a_plugin_module_may_not_shadow_a_core_module(tmp_path):
    import pytest
    from marketplace_lib import MarketplaceError

    catalog = _fixture(tmp_path)
    worker = tmp_path / "packages/demo-plugin/content/worker"
    worker.mkdir(parents=True, exist_ok=True)
    (worker / "plugin_data.py").write_text("", encoding="utf-8")
    manifest_path = tmp_path / "packages/demo-plugin/manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["worker"] = {"module": "plugin_data", "api": "/api/demo-plugin"}
    manifest["entrypoints"]["worker"] = "content/worker/plugin_data.py"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

    with pytest.raises(MarketplaceError, match="shadows a core module"):
        build_app_plugins.worker_payloads(catalog)


def test_a_page_asks_for_camera_and_microphone_through_its_manifest(tmp_path):
    import pytest
    from marketplace_lib import MarketplaceError

    catalog = _fixture(tmp_path)
    manifest_path = tmp_path / "packages/demo-plugin/manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    namespace = {}
    exec(build_app_plugins.registry_source(catalog), namespace)
    assert namespace["PLUGIN_DEVICE_PAGES"] == {}

    manifest["contributions"][0]["config"]["devices"] = ["microphone", "camera"]
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    namespace = {}
    exec(build_app_plugins.registry_source(catalog), namespace)
    assert namespace["PLUGIN_DEVICE_PAGES"] == {
        "/dashboard/demo": ("camera", "microphone"),
    }

    for devices in (["camera", "camera"], ["geolocation"], "camera"):
        manifest["contributions"][0]["config"]["devices"] = devices
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        with pytest.raises(MarketplaceError, match="devices are invalid"):
            build_app_plugins.registry_source(catalog)


def test_shipped_media_pages_are_the_only_device_pages():
    pages = {
        row["route"]: row["devices"]
        for row in build_app_plugins._web_entries() if row["devices"]
    }
    assert pages == {
        "/dashboard/face-changer": ("camera", "microphone"),
        "/dashboard/voice-changer": ("microphone",),
    }

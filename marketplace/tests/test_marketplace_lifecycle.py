#!/usr/bin/env python3
"""Provider-neutral archive and reversible migration behavior."""

import hashlib
import json
import re
import sqlite3
import sys
import zipfile
from pathlib import Path

import pytest


TOOLS = Path(__file__).resolve().parents[1] / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from marketplace_lib import (  # noqa: E402
    MarketplaceError,
    _location,
    install_package,
    load_catalog,
    load_manifest,
    manifest_entries,
    uninstall_package,
)
import generate_desktop_packages  # noqa: E402
import generate_world_packages  # noqa: E402


def _package(root: Path, *, include_down=True):
    (root / "content").mkdir(parents=True)
    (root / "preview").mkdir()
    (root / "migrations").mkdir()
    (root / "content" / "index.js").write_text("export const plugin = true;\n")
    (root / "preview" / "market.png").write_bytes(b"png")
    (root / "migrations" / "0001_up.sql").write_text(
        "CREATE TABLE demo_plugin_items (id INTEGER PRIMARY KEY, value TEXT);\n"
    )
    if include_down:
        (root / "migrations" / "0001_down.sql").write_text(
            "DROP TABLE demo_plugin_items;\n"
        )
    manifest = {
        "schemaVersion": 1,
        "id": "demo-plugin",
        "name": "Demo Plugin",
        "version": "1.2.3",
        "type": "virtual.object",
        "summary": "A provider-neutral package lifecycle test fixture.",
        "creator": {
            "id": "did:forkmesh:tester", "handle": "tester",
            "displayName": "Tester", "verified": False,
        },
        "license": {
            "model": "free-open-source", "spdx": "MIT",
            "creatorRetainsOwnership": True,
        },
        "price": {"amount": 0, "currency": "USD"},
        "entrypoints": {
            "world": "content/index.js",
            "settings": "builtin:package",
        },
        "screenshots": [{"path": "preview/market.png", "alt": "Demo"}],
        "permissions": [],
        "lifecycle": {"migrations": [{
            "id": "0001_create_items",
            "database": "device",
            "up": "migrations/0001_up.sql",
            "down": "migrations/0001_down.sql",
        }]},
    }
    (root / "manifest.json").write_text(json.dumps(manifest))
    return manifest


def _archive(package: Path, destination: Path):
    with zipfile.ZipFile(destination, "w") as bundle:
        for path in sorted(package.rglob("*")):
            if path.is_file():
                bundle.write(path, path.relative_to(package).as_posix())
    body = destination.read_bytes()
    return hashlib.sha256(body).hexdigest(), len(body)


def _catalog(root: Path, artifact: Path, digest: str, size: int):
    payload = {
        "schemaVersion": 2,
        "marketplace": {
            "id": "independent-market",
            "name": "Independent Market",
            "locations": [{
                "transport": "local", "url": ".", "catalog": "catalog.json"
            }],
        },
        "packages": [{
            "id": "demo-plugin",
            "version": "1.2.3",
            # Intentionally absent: an HTTPS catalog carries no core-owned copy.
            "manifest": "packages/demo-plugin/manifest.json",
            "manifestSha256": hashlib.sha256(
                (root / "source" / "manifest.json").read_bytes()
            ).hexdigest(),
            "artifact": {
                "sha256": digest,
                "size": size,
                "locations": [{"transport": "local", "url": str(artifact)}],
            },
        }],
    }
    path = root / "catalog.json"
    path.write_text(json.dumps(payload))
    return load_catalog(path, validate_packages=False)


def test_install_runs_up_and_uninstall_runs_down_in_reverse_capable_host(tmp_path):
    source = tmp_path / "source"
    _package(source)
    archive = tmp_path / "demo.fmpkg"
    digest, size = _archive(source, archive)
    catalog = _catalog(tmp_path, archive, digest, size)
    install_root = tmp_path / "installed"
    database = tmp_path / "device.sqlite"

    installed = install_package(catalog, "demo-plugin", install_root, database)
    assert installed.is_dir()
    with sqlite3.connect(database) as connection:
        assert connection.execute(
            "SELECT migration_id FROM forkmesh_plugin_migrations"
        ).fetchone() == ("0001_create_items",)
        connection.execute("INSERT INTO demo_plugin_items(value) VALUES('works')")

    generated = json.loads((install_root / "catalog.json").read_text())
    assert generated["schemaVersion"] == 2
    assert generated["packages"][0]["id"] == "demo-plugin"
    uninstall_package("demo-plugin", install_root, database)
    assert not installed.exists()
    with sqlite3.connect(database) as connection:
        assert connection.execute(
            "SELECT COUNT(*) FROM forkmesh_plugin_migrations"
        ).fetchone() == (0,)
        with pytest.raises(sqlite3.OperationalError):
            connection.execute("SELECT * FROM demo_plugin_items")


def test_a_migration_without_its_down_file_is_rejected(tmp_path):
    source = tmp_path / "source"
    _package(source, include_down=False)
    with pytest.raises(MarketplaceError, match="down migration does not exist"):
        load_manifest(source / "manifest.json")


def test_instance_scoped_migrations_use_the_same_reversible_lifecycle(tmp_path):
    source = tmp_path / "source"
    manifest = _package(source)
    manifest["lifecycle"]["migrations"][0]["database"] = "organization"
    (source / "manifest.json").write_text(json.dumps(manifest))
    archive = tmp_path / "demo.fmpkg"
    digest, size = _archive(source, archive)
    catalog = _catalog(tmp_path, archive, digest, size)
    install_root = tmp_path / "instance-plugins"
    database = tmp_path / "organization.sqlite"
    install_package(
        catalog, "demo-plugin", install_root, database, "organization"
    )
    with sqlite3.connect(database) as connection:
        assert connection.execute(
            "SELECT scope,migration_id FROM forkmesh_plugin_migrations"
        ).fetchone() == ("organization", "0001_create_items")
    uninstall_package(
        "demo-plugin", install_root, database, "organization"
    )
    with sqlite3.connect(database) as connection:
        assert connection.execute(
            "SELECT COUNT(*) FROM forkmesh_plugin_migrations"
        ).fetchone() == (0,)


def test_digest_mismatch_never_activates_or_migrates_a_package(tmp_path):
    source = tmp_path / "source"
    _package(source)
    archive = tmp_path / "demo.fmpkg"
    _digest, size = _archive(source, archive)
    catalog = _catalog(tmp_path, archive, "0" * 64, size)
    install_root = tmp_path / "installed"
    database = tmp_path / "device.sqlite"
    with pytest.raises(MarketplaceError, match="all package artifact locations failed"):
        install_package(catalog, "demo-plugin", install_root, database)
    assert not (install_root / "packages" / "demo-plugin").exists()
    with sqlite3.connect(database) as connection:
        assert connection.execute(
            "SELECT name FROM sqlite_master WHERE name='demo_plugin_items'"
        ).fetchone() is None


def test_approved_manifest_digest_is_enforced_inside_valid_archive(tmp_path):
    source = tmp_path / "source"
    _package(source)
    archive = tmp_path / "demo.fmpkg"
    digest, size = _archive(source, archive)
    _catalog(tmp_path, archive, digest, size)
    catalog_path = tmp_path / "catalog.json"
    payload = json.loads(catalog_path.read_text())
    payload["packages"][0]["manifestSha256"] = "f" * 64
    catalog_path.write_text(json.dumps(payload))
    catalog = load_catalog(catalog_path, validate_packages=False)
    install_root = tmp_path / "installed"
    with pytest.raises(MarketplaceError, match="approved catalog entry"):
        install_package(catalog, "demo-plugin", install_root)
    assert not (install_root / "packages" / "demo-plugin").exists()


@pytest.mark.parametrize("url,provider", [
    ("https://app.forkmesh.com/alice/plugins.git", "forkmesh"),
    ("https://github.com/alice/plugins.git", "github"),
    ("https://gitlab.com/alice/plugins.git", "gitlab"),
    ("https://code.example.net/alice/plugins.git", "independent"),
])
def test_git_transport_is_provider_neutral(url, provider):
    location = _location({
        "transport": "git",
        "url": url,
        "ref": "v1.2.3",
        "catalog": "marketplace/catalog.json",
        "provider": provider,
    }, "source")
    assert location.transport == "git"
    assert location.url == url
    assert location.provider == provider


def test_core_build_and_world_discovery_are_generated_from_package_metadata():
    repository = Path(__file__).resolve().parents[2]
    catalog = repository / "marketplace" / "catalog.json"
    qrc, cmake, factory_registry = generate_desktop_packages.generate(catalog)
    assert 'alias="catalog.json"' in qrc
    assert "FORKMESH_MARKETPLACE_SOURCES" in cmake
    assert "FORKMESH_MARKETPLACE_TEST_IDS" in cmake
    assert "FORKMESH_PACKAGE_SENTRY_TEST_SOURCES" in cmake
    assert "SentryDesktopExtension.cpp" in cmake
    assert "test_sentry_client.cpp" in cmake
    assert "content/desktop/SentryDesktopExtension.cpp" in factory_registry
    assert "createSentryDesktopExtension" in factory_registry
    core_cmake = (repository / "desktop" / "CMakeLists.txt").read_text()
    for package_id in ("capture", "ledger", "discord", "sentry"):
        assert f"marketplace/packages/{package_id}" not in core_cmake
    assert "sentry" not in core_cmake.lower()
    assert not (repository / "desktop" / "src" / "MainWindowSentry.cpp").exists()
    assert not (repository / "desktop" / "tests" / "test_sentry_client.cpp").exists()
    for path in (repository / "desktop").rglob("*"):
        if not path.is_file() or path.suffix.lower() not in {
            ".cpp", ".h", ".json", ".qrc", ".svg", ".txt"
        }:
            continue
        assert not re.search(r"\bsentry", path.name, re.IGNORECASE), path
        assert not re.search(
            r"\bsentry", path.read_text(errors="ignore"), re.IGNORECASE
        ), path
    core_autogen = (repository / "desktop" / "src" / "forkmesh_autogen.cpp").read_text()
    assert "moc_SentryClient.cpp" not in core_autogen

    generated_path = (
        repository / "world" / "public" / "world" /
        "world-marketplace-packages.js"
    )
    assert generated_path.read_text() == generate_world_packages.generate(
        catalog, generated_path
    )
    scene = (
        repository / "world" / "public" / "world" / "world-scene.js"
    ).read_text()
    assert "packages/x-banner-board" not in scene
    assert "packages/reddit-banner-board" not in scene


def test_desktop_package_tests_can_only_link_allowlisted_qt_targets(tmp_path):
    desktop = tmp_path / "content" / "desktop"
    desktop.mkdir(parents=True)
    (desktop / "Plugin.cpp").write_text("int plugin_value = 1;\n")
    (desktop / "test_plugin.cpp").write_text("int main() { return 0; }\n")
    descriptor = {
        "schemaVersion": 1,
        "sources": ["Plugin.cpp"],
        "mocHeaders": [],
        "testSources": ["test_plugin.cpp"],
        "testLibraries": ["Untrusted::Library"],
    }
    (desktop / "build.json").write_text(json.dumps(descriptor))

    with pytest.raises(MarketplaceError, match="unsupported target"):
        generate_desktop_packages._descriptor(tmp_path)


def test_desktop_package_tests_link_only_named_host_sources(tmp_path):
    desktop = tmp_path / "content" / "desktop"
    desktop.mkdir(parents=True)
    (desktop / "Plugin.cpp").write_text("int plugin_value = 1;\n")
    (desktop / "test_plugin.cpp").write_text("int main() { return 0; }\n")
    descriptor = {
        "schemaVersion": 1,
        "sources": ["Plugin.cpp"],
        "mocHeaders": [],
        "testSources": ["test_plugin.cpp"],
        "testHostSources": ["AgentSonifier.cpp"],
        "testLibraries": ["Qt6::Core"],
    }
    (desktop / "build.json").write_text(json.dumps(descriptor))
    test_sources = generate_desktop_packages._descriptor(tmp_path)[3]
    assert test_sources[-1] == generate_desktop_packages.HOST_SOURCE_DIR / "AgentSonifier.cpp"

    for bad in (["../src/AgentSonifier.cpp"], ["NoSuchFile.cpp"], ["/etc/passwd"]):
        descriptor["testHostSources"] = bad
        (desktop / "build.json").write_text(json.dumps(descriptor))
        with pytest.raises(MarketplaceError):
            generate_desktop_packages._descriptor(tmp_path)


def test_vscode_extension_is_marketplace_owned_and_opt_in():
    repository = Path(__file__).resolve().parents[2]
    catalog = load_catalog(repository / "marketplace" / "catalog.json")
    manifests = {
        manifest["id"]: manifest
        for _entry, manifest in manifest_entries(catalog)
    }
    extension = manifests["forkmesh-ide"]
    package_root = repository / "marketplace" / "packages" / "forkmesh-ide"

    assert extension["type"] == "extension.vscode"
    assert extension["entrypoints"] == {
        "ide": "content/vscode/package.json",
        "settings": "builtin:package",
    }
    assert extension["install"] == {"bundled": True, "default": False}
    assert (package_root / extension["entrypoints"]["ide"]).is_file()
    assert (package_root / "content/vscode/src/extension.ts").is_file()
    assert not (repository / "extensions").exists()


def test_desktop_packages_embed_only_named_repository_folders(tmp_path):
    desktop = tmp_path / "content" / "desktop"
    desktop.mkdir(parents=True)
    (desktop / "Plugin.cpp").write_text("int plugin_value = 1;\n")
    descriptor = {
        "schemaVersion": 1,
        "sources": ["Plugin.cpp"],
        "mocHeaders": [],
        "repositoryResources": ["www/docs"],
    }
    (desktop / "build.json").write_text(json.dumps(descriptor))
    resources = generate_desktop_packages._descriptor(tmp_path)[6]
    assert resources == [generate_desktop_packages.REPOSITORY_ROOT / "www" / "docs"]

    for bad in (["../etc"], ["/etc"], ["www/no-such-folder"], ["www/docs/README.md"], "www/docs"):
        descriptor["repositoryResources"] = bad
        (desktop / "build.json").write_text(json.dumps(descriptor))
        with pytest.raises(MarketplaceError):
            generate_desktop_packages._descriptor(tmp_path)


def test_docs_package_bundles_the_web_library():
    repository = Path(__file__).resolve().parents[2]
    catalog = repository / "marketplace" / "catalog.json"
    qrc, cmake, _ = generate_desktop_packages.generate(catalog)
    assert 'alias="packages/docs/repository/www/docs/README.md"' in qrc
    assert 'alias="packages/docs/repository/www/docs/terminology.json"' in qrc
    assert f'"{(repository / "www" / "docs").as_posix()}/*"' in cmake
    manifest = load_manifest(repository / "marketplace" / "packages" / "docs" / "manifest.json")
    assert manifest["entrypoints"]["desktop"] == "builtin:docs"
    assert manifest["install"] == {
        "bundled": True, "default": True, "sync": "account", "places": ["mobile", "desktop"],
    }
    assert manifest["entrypoints"]["ios"] == manifest["entrypoints"]["android"] == "builtin:docs"


def _local_catalog(root: Path):
    payload = {
        "schemaVersion": 2,
        "marketplace": {
            "id": "independent-market",
            "name": "Independent Market",
            "locations": [{
                "transport": "local", "url": ".", "catalog": "catalog.json"
            }],
        },
        "packages": [{
            "id": "demo-plugin",
            "version": "1.2.3",
            "manifest": "source/manifest.json",
        }],
    }
    path = root / "catalog.json"
    path.write_text(json.dumps(payload))
    return load_catalog(path, validate_packages=False)


@pytest.mark.parametrize("escape", [
    "END;",
    "COMMIT TRANSACTION; END;",
])
def test_a_migration_cannot_end_the_host_transaction_and_attach_files(tmp_path, escape):
    source = tmp_path / "source"
    _package(source)
    planted = tmp_path / "planted.sqlite"
    (source / "migrations" / "0001_up.sql").write_text(
        "CREATE TABLE demo_plugin_items (id INTEGER PRIMARY KEY, value TEXT);\n"
        f"{escape}\nATTACH DATABASE '{planted}' AS planted;\n"
        "CREATE TABLE planted.loot (x);\n"
    )
    catalog = _local_catalog(tmp_path)
    with pytest.raises(MarketplaceError, match="controls transactions"):
        install_package(catalog, "demo-plugin", tmp_path / "installed",
                        tmp_path / "device.sqlite")
    assert not planted.exists()


def test_install_refuses_a_package_folder_with_symlinks(tmp_path):
    source = tmp_path / "source"
    _package(source)
    secret = tmp_path / "secret.txt"
    secret.write_text("node identity")
    (source / "content" / "leak.txt").symlink_to(secret)
    catalog = _local_catalog(tmp_path)
    install_root = tmp_path / "installed"
    with pytest.raises(MarketplaceError, match="symlink"):
        install_package(catalog, "demo-plugin", install_root,
                        tmp_path / "device.sqlite")
    assert not list(install_root.rglob("leak.txt"))

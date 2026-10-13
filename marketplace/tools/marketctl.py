#!/usr/bin/env python3
"""Validate, mirror, pack, install, and uninstall ForkMesh packages."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile

from marketplace_lib import (
    Location,
    MARKETPLACE_ID_RE,
    MarketplaceError,
    _location,
    install_package,
    load_catalog,
    load_manifest,
    manifest_entries,
    sync_marketplace,
    uninstall_package,
)


def validate(path: Path) -> None:
    if path.is_dir():
        candidate = path / "catalog.json"
        if not candidate.is_file():
            candidate = path / "manifest.json"
        path = candidate
    if path.name == "catalog.json":
        catalog = load_catalog(path)
        packages = list(manifest_entries(catalog))
        print(
            f"valid marketplace {catalog.marketplace_id}: "
            f"{len(packages)} package(s)"
        )
    else:
        manifest = load_manifest(path)
        print(f"valid package {manifest['id']} {manifest['version']}")


def pack(package_root: Path, output: Path) -> None:
    package_root = package_root.resolve()
    manifest = load_manifest(package_root / "manifest.json")
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(package_root.rglob("*")):
            if path.is_file():
                relative = path.relative_to(package_root).as_posix()
                info = zipfile.ZipInfo(relative, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, path.read_bytes())
    body = output.read_bytes()
    print(
        json.dumps(
            {
                "package": manifest["id"],
                "version": manifest["version"],
                "artifact": str(output),
                "size": len(body),
                "sha256": hashlib.sha256(body).hexdigest(),
            },
            indent=2,
        )
    )


def load_source_file(path: Path) -> list[tuple[str, list[Location]]]:
    try:
        root = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise MarketplaceError(f"could not read source registry: {error}") from error
    if not isinstance(root, dict) or root.get("schemaVersion") != 1:
        raise MarketplaceError("source registry must use schemaVersion 1")
    sources = root.get("marketplaces")
    if not isinstance(sources, list):
        raise MarketplaceError("source registry marketplaces must be an array")
    parsed = []
    for source_index, source in enumerate(sources):
        if not isinstance(source, dict):
            raise MarketplaceError(f"marketplace source {source_index} is invalid")
        source_id = str(source.get("id") or "")
        raw_locations = source.get("locations")
        if (
            not MARKETPLACE_ID_RE.fullmatch(source_id)
            or len(source_id) > 96
            or not isinstance(raw_locations, list)
            or not raw_locations
        ):
            raise MarketplaceError(f"marketplace source {source_index} is incomplete")
        parsed.append(
            (
                source_id,
                [
                    _location(value, f"{source_id} location {index}")
                    for index, value in enumerate(raw_locations)
                ],
            )
        )
    return parsed


def sync_sources(registry: Path, cache: Path) -> None:
    sources = load_source_file(registry)
    for source_id, locations in sources:
        catalog_path = sync_marketplace(
            locations, cache / source_id, registry.resolve().parent
        )
        catalog = load_catalog(catalog_path, validate_packages=False)
        if catalog.marketplace_id != source_id:
            raise MarketplaceError(
                f"source id {source_id} resolved marketplace {catalog.marketplace_id}"
            )
        print(f"synced {source_id}: {catalog_path}")


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(
        description=(
            "ForkMesh marketplace tooling. Git sources work with GitHub, GitLab, "
            "any Git host, and ForkMesh repositories/mirrors without provider APIs."
        )
    )
    commands = root.add_subparsers(dest="command", required=True)
    check = commands.add_parser("validate", help="validate a catalog or package")
    check.add_argument("path", type=Path)
    bundle = commands.add_parser("pack", help="build a deterministic .fmpkg input")
    bundle.add_argument("package", type=Path)
    bundle.add_argument("output", type=Path)
    sync = commands.add_parser("sync", help="sync configured marketplace mirrors")
    sync.add_argument("registry", type=Path)
    sync.add_argument("cache", type=Path)
    install = commands.add_parser("install", help="install one package")
    install.add_argument("package")
    install.add_argument("--catalog", required=True, type=Path)
    install.add_argument("--install-root", required=True, type=Path)
    install.add_argument("--database", type=Path)
    install.add_argument(
        "--database-scope", choices=("device", "organization"),
        default="device",
    )
    remove = commands.add_parser(
        "uninstall", help="run down migrations and remove one package"
    )
    remove.add_argument("package")
    remove.add_argument("--install-root", required=True, type=Path)
    remove.add_argument("--database", type=Path)
    remove.add_argument(
        "--database-scope", choices=("device", "organization"),
        default="device",
    )
    return root


def main(arguments: list[str]) -> int:
    options = parser().parse_args(arguments)
    try:
        if options.command == "validate":
            validate(options.path)
        elif options.command == "pack":
            pack(options.package, options.output)
        elif options.command == "sync":
            sync_sources(options.registry, options.cache)
        elif options.command == "install":
            # HTTPS catalogs intentionally do not bake third-party manifests
            # into ForkMesh. The signed artifact is downloaded and its
            # manifest identity is verified during installation.
            catalog = load_catalog(options.catalog, validate_packages=False)
            database = options.database or (
                options.install_root / "plugin-state.sqlite"
            )
            target = install_package(
                catalog, options.package, options.install_root, database,
                options.database_scope,
            )
            print(f"installed {options.package}: {target}")
        elif options.command == "uninstall":
            database = options.database or (
                options.install_root / "plugin-state.sqlite"
            )
            uninstall_package(
                options.package, options.install_root, database,
                options.database_scope,
            )
            print(f"uninstalled {options.package}")
    except MarketplaceError as error:
        print(f"marketctl: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

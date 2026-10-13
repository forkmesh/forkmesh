#!/usr/bin/env python3
"""Generate Qt resources and build inputs from package-owned descriptors."""

from __future__ import annotations

import argparse
import html
import json
from pathlib import Path
import re
import sys

from marketplace_lib import (
    MarketplaceError, load_catalog, load_onboarding, manifest_entries, safe_relative_path,
)


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
RESOURCE_EXCLUDES = {"build.json", "CMakeLists.txt"}
TEST_LIBRARIES = {
    "Qt6::Core",
    "Qt6::Network",
    "Qt6::Widgets",
    "Qt6::Svg",
    "Qt6::Concurrent",
    # The application already links it; a package whose core is cryptography
    # tests that core against the same library.
    "OpenSSL::Crypto",
}
REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
HOST_SOURCE_DIR = REPOSITORY_ROOT / "desktop" / "src"
HOST_SOURCE_NAME = re.compile(r"[A-Za-z][A-Za-z0-9_]*\.(?:h|cpp)")


def _descriptor(
    package_root: Path,
) -> tuple[list[Path], list[Path], Path, list[Path], list[str], str, list[Path]] | None:
    descriptor_path = package_root / "content" / "desktop" / "build.json"
    if not descriptor_path.is_file():
        return None
    try:
        descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise MarketplaceError(f"could not read {descriptor_path}: {error}") from error
    if not isinstance(descriptor, dict) or descriptor.get("schemaVersion") != 1:
        raise MarketplaceError(f"{descriptor_path} must use schemaVersion 1")
    if set(descriptor) - {
        "schemaVersion", "sources", "mocHeaders", "testSources", "testLibraries",
        "testHostSources", "desktopFactory", "repositoryResources",
    }:
        raise MarketplaceError(f"{descriptor_path} has unsupported fields")
    source_dir = descriptor_path.parent.resolve()

    def paths(key: str, *, optional: bool = False) -> list[Path]:
        values = descriptor.get(key, [] if optional else None)
        if not isinstance(values, list):
            raise MarketplaceError(f"{descriptor_path} {key} must be an array")
        resolved = []
        for value in values:
            relative = safe_relative_path(value, f"{descriptor_path} {key}")
            target = (source_dir / relative).resolve()
            try:
                target.relative_to(source_dir)
            except ValueError as error:
                raise MarketplaceError(
                    f"{descriptor_path} references file outside its desktop content: {relative}"
                ) from error
            if not target.is_file():
                raise MarketplaceError(f"{descriptor_path} references missing {relative}")
            resolved.append(target)
        return resolved

    sources = paths("sources")
    moc_headers = paths("mocHeaders")
    test_sources = paths("testSources", optional=True)
    test_libraries = descriptor.get("testLibraries", [])
    if not isinstance(test_libraries, list) or any(
        not isinstance(library, str) or library not in TEST_LIBRARIES
        for library in test_libraries
    ):
        raise MarketplaceError(
            f"{descriptor_path} testLibraries contains an unsupported target"
        )
    if bool(test_sources) != bool(test_libraries):
        raise MarketplaceError(
            f"{descriptor_path} testSources and testLibraries must be declared together"
        )
    host_sources = descriptor.get("testHostSources", [])
    if not isinstance(host_sources, list) or any(
        not isinstance(name, str) or not HOST_SOURCE_NAME.fullmatch(name)
        for name in host_sources
    ):
        raise MarketplaceError(
            f"{descriptor_path} testHostSources must name files in desktop/src"
        )
    if host_sources and not test_sources:
        raise MarketplaceError(
            f"{descriptor_path} testHostSources needs testSources"
        )
    for name in host_sources:
        target = HOST_SOURCE_DIR / name
        if not target.is_file():
            raise MarketplaceError(f"{descriptor_path} references missing host {name}")
        test_sources.append(target)
    if not set(moc_headers).issubset(sources):
        raise MarketplaceError(f"{descriptor_path} mocHeaders must also be sources")
    desktop_factory = descriptor.get("desktopFactory", "")
    if not isinstance(desktop_factory, str) or (
        desktop_factory and not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", desktop_factory)
    ):
        raise MarketplaceError(
            f"{descriptor_path} desktopFactory must be a C++ identifier"
        )
    repository_resources = descriptor.get("repositoryResources", [])
    if not isinstance(repository_resources, list):
        raise MarketplaceError(f"{descriptor_path} repositoryResources must be an array")
    resource_dirs = []
    for value in repository_resources:
        relative = safe_relative_path(value, f"{descriptor_path} repositoryResources")
        target = (REPOSITORY_ROOT / relative).resolve()
        try:
            target.relative_to(REPOSITORY_ROOT)
        except ValueError as error:
            raise MarketplaceError(
                f"{descriptor_path} repositoryResources leaves the repository: {relative}"
            ) from error
        if not target.is_dir():
            raise MarketplaceError(
                f"{descriptor_path} repositoryResources names a missing folder: {relative}"
            )
        resource_dirs.append(target)
    return (
        sources, moc_headers, source_dir, test_sources, test_libraries,
        desktop_factory, resource_dirs,
    )


def generate(catalog_path: Path) -> tuple[str, str, str]:
    catalog = load_catalog(catalog_path)
    resource_paths = {
        catalog.path,
        catalog.root / "catalog.schema.json",
        catalog.root / "manifest.schema.json",
        catalog.root / "agent.schema.json",
    }
    packs, guides = load_onboarding(catalog)
    if packs:
        resource_paths.add(catalog.root / "packs.json")
    if guides:
        resource_paths.add(catalog.root / "guides.json")
    resource_aliases: dict[Path, str] = {}
    configure_globs: list[str] = []
    sources: list[Path] = []
    moc_headers: list[Path] = []
    include_dirs: list[Path] = []
    package_sources: dict[str, list[Path]] = {}
    package_tests: dict[str, tuple[list[Path], list[str]]] = {}
    desktop_factories: list[tuple[str, str]] = []
    for entry, manifest in manifest_entries(catalog):
        package_root = (catalog.root / entry.manifest).parent.resolve()
        descriptor = _descriptor(package_root)
        if descriptor:
            (
                package_source_files,
                package_moc,
                include_dir,
                package_test_sources,
                package_test_libraries,
                desktop_factory,
                resource_dirs,
            ) = descriptor
            package_alias = package_root.relative_to(catalog.root.resolve()).as_posix()
            for resource_dir in resource_dirs:
                repository_alias = resource_dir.relative_to(REPOSITORY_ROOT).as_posix()
                configure_globs.append(f"{resource_dir.as_posix()}/*")
                for path in sorted(resource_dir.rglob("*")):
                    if path.is_file() and not any(
                        part.startswith(".") for part in path.relative_to(resource_dir).parts
                    ):
                        try:
                            path.resolve().relative_to(REPOSITORY_ROOT)
                        except ValueError as error:
                            raise MarketplaceError(
                                f"{manifest['id']} repository resource leaves the repository: {path}"
                            ) from error
                        resource_aliases[path.resolve()] = (
                            f"{package_alias}/repository/{repository_alias}/"
                            f"{path.relative_to(resource_dir).as_posix()}"
                        )
            sources.extend(package_source_files)
            moc_headers.extend(package_moc)
            include_dirs.append(include_dir)
            package_sources[manifest["id"]] = package_source_files
            if package_test_sources:
                package_tests[manifest["id"]] = (
                    package_test_sources,
                    package_test_libraries,
                )
            if desktop_factory:
                entrypoint = manifest.get("entrypoints", {}).get("desktop", "")
                if not entrypoint or entrypoint.startswith("builtin:"):
                    raise MarketplaceError(
                        f"{manifest['id']} desktopFactory requires a content desktop entrypoint"
                    )
                desktop_factories.append((entrypoint, desktop_factory))
        for path in package_root.rglob("*"):
            if (
                path.is_file()
                and path.name not in RESOURCE_EXCLUDES
                and path.suffix.lower() not in SOURCE_SUFFIXES
                and "tests" not in path.relative_to(package_root).parts
            ):
                resource_paths.add(path.resolve())

    missing_resources = sorted(str(path) for path in resource_paths if not path.is_file())
    if missing_resources:
        raise MarketplaceError("missing marketplace resources: " + ", ".join(missing_resources))
    entrypoints = [entrypoint for entrypoint, _ in desktop_factories]
    if len(entrypoints) != len(set(entrypoints)):
        raise MarketplaceError("desktop package entrypoints must be unique")
    factories = [factory for _, factory in desktop_factories]
    if len(factories) != len(set(factories)):
        raise MarketplaceError("desktop package factory symbols must be unique")

    qrc = ["<RCC>", '  <qresource prefix="/forkmesh-market">']
    entries = [(path.relative_to(catalog.root).as_posix(), path) for path in resource_paths]
    entries += [(alias, path) for path, alias in resource_aliases.items()]
    for alias, path in sorted(entries):
        qrc.append(
            f'    <file alias="{html.escape(alias, quote=True)}">'
            f"{html.escape(str(path), quote=True)}</file>"
        )
    qrc += ["  </qresource>", "</RCC>", ""]

    def cmake_set(name: str, values: list[Path | str]) -> list[str]:
        rows = [f"set({name}"]
        rows += [
            f'    "{value.as_posix() if isinstance(value, Path) else value}"'
            for value in dict.fromkeys(values)
        ]
        rows.append(")")
        return rows

    cmake = [
        "# Generated by marketplace/tools/generate_desktop_packages.py; do not edit.",
        *cmake_set("FORKMESH_MARKETPLACE_SOURCES", sources),
        *cmake_set("FORKMESH_MARKETPLACE_MOC_HEADERS", moc_headers),
        *cmake_set("FORKMESH_MARKETPLACE_INCLUDE_DIRS", include_dirs),
        *cmake_set("FORKMESH_MARKETPLACE_TEST_IDS", list(package_tests)),
        *cmake_set("FORKMESH_MARKETPLACE_RESOURCE_GLOBS", configure_globs),
    ]
    for package_id, values in sorted(package_sources.items()):
        variable_id = re.sub(r"[^A-Za-z0-9]", "_", package_id).upper()
        cmake += cmake_set(f"FORKMESH_PACKAGE_{variable_id}_SOURCES", values)
    for package_id, (test_sources, test_libraries) in sorted(package_tests.items()):
        variable_id = re.sub(r"[^A-Za-z0-9]", "_", package_id).upper()
        cmake += cmake_set(
            f"FORKMESH_PACKAGE_{variable_id}_TEST_SOURCES", test_sources
        )
        cmake += cmake_set(
            f"FORKMESH_PACKAGE_{variable_id}_TEST_LIBRARIES", test_libraries
        )
    cmake.append("")
    registry = [
        "// Generated by marketplace/tools/generate_desktop_packages.py; do not edit.",
        '#include "DesktopPackageExtension.h"',
        "",
        "namespace forkmesh::market {",
    ]
    for _, factory in desktop_factories:
        registry.append(f"DesktopPackageExtension *{factory}();")
    registry += [
        "",
        "std::unique_ptr<DesktopPackageExtension>",
        "createDesktopPackageExtension(const QString &entrypoint)",
        "{",
    ]
    for entrypoint, factory in desktop_factories:
        literal = json.dumps(entrypoint, ensure_ascii=True)
        registry += [
            f"    if (entrypoint == QStringLiteral({literal}))",
            f"        return std::unique_ptr<DesktopPackageExtension>({factory}());",
        ]
    registry += [
        "    return {};",
        "}",
        "",
        "bool hasDesktopPackageExtension(const QString &entrypoint)",
        "{",
    ]
    if desktop_factories:
        comparisons = [
            f"entrypoint == QStringLiteral({json.dumps(entrypoint, ensure_ascii=True)})"
            for entrypoint, _ in desktop_factories
        ]
        registry.append("    return " + " ||\n           ".join(comparisons) + ";")
    else:
        registry.append("    Q_UNUSED(entrypoint);\n    return false;")
    registry += ["}", "", "} // namespace forkmesh::market", ""]
    return "\n".join(qrc), "\n".join(cmake), "\n".join(registry)


def main(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--qrc", required=True, type=Path)
    parser.add_argument("--cmake", required=True, type=Path)
    parser.add_argument("--registry-cpp", required=True, type=Path)
    options = parser.parse_args(arguments)
    try:
        qrc, cmake, registry = generate(options.catalog)
    except MarketplaceError as error:
        print(f"generate_desktop_packages: {error}", file=sys.stderr)
        return 2
    options.qrc.parent.mkdir(parents=True, exist_ok=True)
    options.cmake.parent.mkdir(parents=True, exist_ok=True)
    options.qrc.write_text(qrc, encoding="utf-8")
    options.cmake.write_text(cmake, encoding="utf-8")
    options.registry_cpp.parent.mkdir(parents=True, exist_ok=True)
    options.registry_cpp.write_text(registry, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

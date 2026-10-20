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
    MarketplaceError, load_catalog, load_onboarding, plugin_selection, safe_relative_path,
    selected_catalog_document, selected_manifest_entries, selected_onboarding_documents,
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
LISTING_FILES = {"manifest.json", "README.md", "LICENSE"}


def _listing_file(relative: Path) -> bool:
    return relative.parts[0] == "preview" or (
        len(relative.parts) == 1 and relative.name in LISTING_FILES
    )


def _qrc(entries: list[tuple[str, Path]]) -> str:
    rows = ["<RCC>", '  <qresource prefix="/forkmesh-market">']
    for alias, path in sorted(entries):
        rows.append(
            f'    <file alias="{html.escape(alias, quote=True)}">'
            f"{html.escape(str(path), quote=True)}</file>"
        )
    rows += ["  </qresource>", "</RCC>", ""]
    return "\n".join(rows)


def _descriptor(
    package_root: Path,
) -> tuple[list[Path], list[Path], Path, list[Path], list[str], bool, list[Path]] | None:
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
        "testHostSources", "coreLinked", "repositoryResources",
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
    core_linked = descriptor.get("coreLinked", False)
    if not isinstance(core_linked, bool):
        raise MarketplaceError(f"{descriptor_path} coreLinked must be true or false")
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
        core_linked, resource_dirs,
    )


def core_linked_plugins(catalog_path: Path) -> list[str]:
    catalog = load_catalog(catalog_path)
    linked = []
    for entry in catalog.packages:
        descriptor = _descriptor((catalog.root / entry.manifest).parent.resolve())
        if descriptor and descriptor[5]:
            linked.append(entry.package_id)
    return linked


def generate(
    catalog_path: Path, plugins: str = "", generated_dir: Path | None = None,
) -> tuple[str, str, dict[str, str], dict[str, str]]:
    catalog = load_catalog(catalog_path)
    load_onboarding(catalog)
    selection = plugin_selection(catalog, plugins) | set(core_linked_plugins(catalog_path))
    selection = plugin_selection(catalog, ";".join(sorted(selection)) or "none")
    generated_dir = generated_dir or catalog.root
    documents = {"catalog.json": json.dumps(
        selected_catalog_document(catalog, selection), indent=2) + "\n"}
    packs, guides = selected_onboarding_documents(catalog, selection)
    if packs:
        documents["packs.json"] = json.dumps(packs, indent=2) + "\n"
    if guides:
        documents["guides.json"] = json.dumps(guides, indent=2) + "\n"
    resource_paths = {
        catalog.root / "catalog.schema.json",
        catalog.root / "manifest.schema.json",
        catalog.root / "agent.schema.json",
    }
    resource_aliases: dict[Path, str] = {
        (generated_dir / name).resolve(): name for name in documents
    }
    configure_globs: list[str] = []
    sources: list[Path] = []
    moc_headers: list[Path] = []
    include_dirs: list[Path] = []
    package_sources: dict[str, list[Path]] = {}
    package_tests: dict[str, tuple[list[Path], list[str]]] = {}
    module_candidates: list[str] = []
    core_linked: list[str] = []
    module_resources: dict[str, list[tuple[str, Path]]] = {}
    for entry, manifest in selected_manifest_entries(catalog, selection):
        package_root = (catalog.root / entry.manifest).parent.resolve()
        descriptor = _descriptor(package_root)
        if descriptor:
            (
                package_source_files,
                package_moc,
                include_dir,
                package_test_sources,
                package_test_libraries,
                linked,
                resource_dirs,
            ) = descriptor
            if not linked:
                module_resources[manifest["id"]] = []
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
                        alias = (
                            f"{package_alias}/repository/{repository_alias}/"
                            f"{path.relative_to(resource_dir).as_posix()}"
                        )
                        if linked:
                            resource_aliases[path.resolve()] = alias
                        else:
                            module_resources[manifest["id"]].append((alias, path.resolve()))
            sources.extend(package_source_files)
            moc_headers.extend(package_moc)
            include_dirs.append(include_dir)
            package_sources[manifest["id"]] = package_source_files
            if package_test_sources:
                package_tests[manifest["id"]] = (
                    package_test_sources,
                    package_test_libraries,
                )
            (core_linked if linked else module_candidates).append(manifest["id"])
        for path in package_root.rglob("*"):
            if (
                path.is_file()
                and path.name not in RESOURCE_EXCLUDES
                and path.suffix.lower() not in SOURCE_SUFFIXES
                and "tests" not in path.relative_to(package_root).parts
            ):
                relative = path.relative_to(package_root)
                if manifest["id"] in module_resources and not _listing_file(relative):
                    module_resources[manifest["id"]].append(
                        (path.resolve().relative_to(catalog.root.resolve()).as_posix(),
                         path.resolve()))
                else:
                    resource_paths.add(path.resolve())

    missing_resources = sorted(str(path) for path in resource_paths if not path.is_file())
    if missing_resources:
        raise MarketplaceError("missing marketplace resources: " + ", ".join(missing_resources))

    entries = [(path.relative_to(catalog.root).as_posix(), path) for path in resource_paths]
    entries += [(alias, path) for path, alias in resource_aliases.items()]
    qrc = _qrc(entries)
    plugin_qrcs = {
        package_id: _qrc(files) for package_id, files in module_resources.items() if files
    }

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
        *cmake_set("FORKMESH_SELECTED_PLUGINS", sorted(selection)),
        *cmake_set("FORKMESH_PLUGIN_MODULE_CANDIDATES", module_candidates),
        *cmake_set("FORKMESH_CORE_LINKED_PLUGINS", core_linked),
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
    for package_id in sorted(plugin_qrcs):
        variable_id = re.sub(r"[^A-Za-z0-9]", "_", package_id).upper()
        cmake.append(
            f'set(FORKMESH_PACKAGE_{variable_id}_QRC '
            f'"{(generated_dir / "plugins" / (package_id + ".qrc")).resolve().as_posix()}")'
        )
    cmake.append("")
    return qrc, "\n".join(cmake), documents, plugin_qrcs


def main(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--qrc", required=True, type=Path)
    parser.add_argument("--cmake", required=True, type=Path)
    parser.add_argument("--plugins", default="")
    options = parser.parse_args(arguments)
    generated_dir = options.qrc.parent
    try:
        qrc, cmake, documents, plugin_qrcs = generate(
            options.catalog, options.plugins, generated_dir)
    except MarketplaceError as error:
        print(f"generate_desktop_packages: {error}", file=sys.stderr)
        return 2
    generated_dir.mkdir(parents=True, exist_ok=True)
    options.cmake.parent.mkdir(parents=True, exist_ok=True)
    for name in ("catalog.json", "packs.json", "guides.json"):
        target = generated_dir / name
        if name in documents:
            if not target.is_file() or target.read_text(encoding="utf-8") != documents[name]:
                target.write_text(documents[name], encoding="utf-8")
        elif target.is_file():
            target.unlink()
    plugin_dir = generated_dir / "plugins"
    plugin_dir.mkdir(exist_ok=True)
    for stale in plugin_dir.glob("*.qrc"):
        if stale.stem not in plugin_qrcs:
            stale.unlink()
    for package_id, text in plugin_qrcs.items():
        target = plugin_dir / (package_id + ".qrc")
        if not target.is_file() or target.read_text(encoding="utf-8") != text:
            target.write_text(text, encoding="utf-8")
    if not options.qrc.is_file() or options.qrc.read_text(encoding="utf-8") != qrc:
        options.qrc.write_text(qrc, encoding="utf-8")
    options.cmake.write_text(cmake, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

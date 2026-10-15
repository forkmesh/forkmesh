"""Provider-neutral ForkMesh marketplace and package lifecycle primitives.

The module intentionally uses only the Python standard library so the desktop
build, an independently hosted ForkMesh instance, and a package author can run
the same validation and install code without a ForkMesh service dependency.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import sqlite3
import subprocess
import tempfile
from typing import Iterable
from urllib.parse import urlparse
from urllib.request import Request, urlopen
import zipfile


PACKAGE_ID_RE = re.compile(r"^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*$")
MARKETPLACE_ID_RE = PACKAGE_ID_RE
VERSION_RE = re.compile(
    r"^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$"
)
MIGRATION_ID_RE = re.compile(r"^[0-9]{4}_[a-z][a-z0-9_-]*$")
SETTING_KEY_RE = re.compile(r"^[a-z][A-Za-z0-9]{0,39}$")
INSTALLED_BEFORE_RE = re.compile(r"^20[0-9]{2}-(?:0[1-9]|1[0-2])-(?:0[1-9]|[12][0-9]|3[01])$")
SETTING_VALUE_RE = re.compile(r"^[A-Za-z0-9._-]{1,40}$")
TRANSACTION_SQL_RE = re.compile(
    r"\b(?:BEGIN|COMMIT|END|ROLLBACK|SAVEPOINT|RELEASE|ATTACH|DETACH|VACUUM)\b",
    re.IGNORECASE,
)
MAX_ARCHIVE_BYTES = 512 * 1024 * 1024
MAX_ARCHIVE_FILES = 4096
PACKAGE_TYPES = {
    "extension.native", "extension.vscode", "virtual.world", "virtual.avatar",
    "virtual.object", "virtual.clothing", "virtual.animation", "virtual.audio",
    "agent", "service.agent", "service.development", "service.design",
    "service.hosting", "service.compute", "theme",
}
LICENSE_MODELS = {
    "free-open-source", "one-time", "commercial", "subscription",
    "pay-what-you-want", "limited-edition", "free-with-paid-source",
    "revenue-share",
}


class MarketplaceError(ValueError):
    """A catalog, source, archive, manifest, or lifecycle is unsafe/invalid."""


@dataclass(frozen=True)
class Location:
    transport: str
    url: str
    ref: str = ""
    catalog: str = "catalog.json"
    provider: str = ""


@dataclass(frozen=True)
class PackageEntry:
    package_id: str
    version: str
    marketplace_id: str
    manifest: str
    manifest_sha256: str = ""
    sources: tuple[dict, ...] = ()
    artifact: dict | None = None


@dataclass(frozen=True)
class Catalog:
    path: Path
    marketplace_id: str
    name: str
    locations: tuple[Location, ...]
    packages: tuple[PackageEntry, ...]

    @property
    def root(self) -> Path:
        return self.path.parent


def _object(value, label: str) -> dict:
    if not isinstance(value, dict):
        raise MarketplaceError(f"{label} must be an object")
    return value


def safe_relative_path(value: object, label: str = "path") -> str:
    text = str(value or "")
    path = PurePosixPath(text)
    if (
        not text
        or len(text) > 320
        or text.startswith(("/", "\\"))
        or "\\" in text
        or path.is_absolute()
        or any(part in ("", ".", "..") for part in path.parts)
        or not re.fullmatch(r"[A-Za-z0-9._/-]+", text)
    ):
        raise MarketplaceError(f"unsafe {label}: {text!r}")
    return text


def _location(value: object, label: str) -> Location:
    raw = _object(value, label)
    unknown = set(raw) - {"transport", "url", "ref", "catalog", "provider"}
    if unknown:
        raise MarketplaceError(f"{label} has unsupported fields: {sorted(unknown)}")
    transport = str(raw.get("transport") or "")
    url = str(raw.get("url") or "")
    ref = str(raw.get("ref") or "")
    catalog = str(raw.get("catalog") or "catalog.json")
    provider = str(raw.get("provider") or "")
    if transport not in {"local", "git", "https"}:
        raise MarketplaceError(f"{label} has unsupported transport {transport!r}")
    if not url or len(url) > 2048:
        raise MarketplaceError(f"{label} has an invalid URL")
    if ref and (len(ref) > 200 or ref.startswith("-")):
        raise MarketplaceError(f"{label} has an unsafe Git ref")
    safe_relative_path(catalog, f"{label} catalog")
    if transport == "https":
        parsed = urlparse(url)
        if parsed.scheme != "https" or not parsed.hostname or parsed.username:
            raise MarketplaceError(f"{label} must use an absolute HTTPS URL")
    if transport == "git":
        parsed = urlparse(url.removeprefix("git+"))
        is_scp = bool(re.fullmatch(r"[^/@:]+@[^/:]+:.+", url))
        if not is_scp and (
            parsed.scheme not in {"https", "ssh", "git"}
            or not parsed.hostname
            or parsed.username and parsed.scheme == "https"
        ):
            raise MarketplaceError(f"{label} is not a supported Git URL")
    return Location(transport, url, ref, catalog, provider)


def load_catalog(path: Path | str, *, validate_packages: bool = True) -> Catalog:
    catalog_path = Path(path).resolve()
    try:
        raw = json.loads(catalog_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise MarketplaceError(f"could not read catalog {catalog_path}: {error}") from error
    root = _object(raw, "catalog")
    if root.get("schemaVersion") != 2:
        raise MarketplaceError("marketplace catalogs must use schemaVersion 2")
    market = _object(root.get("marketplace"), "catalog marketplace")
    market_id = str(market.get("id") or "")
    name = str(market.get("name") or "")
    if not MARKETPLACE_ID_RE.fullmatch(market_id) or len(market_id) > 96:
        raise MarketplaceError("catalog marketplace id is invalid")
    if not name or len(name) > 80:
        raise MarketplaceError("catalog marketplace name is invalid")
    raw_locations = market.get("locations")
    if not isinstance(raw_locations, list) or not 1 <= len(raw_locations) <= 16:
        raise MarketplaceError("catalog marketplace needs 1-16 source locations")
    locations = tuple(
        _location(value, f"marketplace location {index}")
        for index, value in enumerate(raw_locations)
    )
    raw_packages = root.get("packages")
    if not isinstance(raw_packages, list):
        raise MarketplaceError("catalog packages must be an array")
    entries: list[PackageEntry] = []
    ids: set[str] = set()
    manifests: dict[str, dict] = {}
    for index, value in enumerate(raw_packages):
        entry = _object(value, f"package entry {index}")
        unknown = set(entry) - {
            "id", "version", "marketplace", "manifest", "manifestSha256",
            "sources", "artifact"
        }
        if unknown:
            raise MarketplaceError(
                f"package entry {index} has unsupported fields: {sorted(unknown)}"
            )
        package_id = str(entry.get("id") or "")
        version = str(entry.get("version") or "")
        if not PACKAGE_ID_RE.fullmatch(package_id) or len(package_id) > 96:
            raise MarketplaceError(f"package entry {index} id is invalid")
        if not VERSION_RE.fullmatch(version) or len(version) > 80:
            raise MarketplaceError(f"package entry {index} version is invalid")
        if package_id in ids:
            raise MarketplaceError(f"duplicate package id: {package_id}")
        ids.add(package_id)
        entry_marketplace_id = str(entry.get("marketplace") or market_id)
        if (
            not MARKETPLACE_ID_RE.fullmatch(entry_marketplace_id)
            or len(entry_marketplace_id) > 96
        ):
            raise MarketplaceError(
                f"package entry {index} marketplace provenance is invalid"
            )
        manifest_path = safe_relative_path(
            entry.get("manifest"), f"package entry {index} manifest"
        )
        manifest_sha256 = str(entry.get("manifestSha256") or "")
        if manifest_sha256 and not re.fullmatch(r"[0-9a-f]{64}", manifest_sha256):
            raise MarketplaceError(
                f"package entry {index} manifest SHA-256 is invalid"
            )
        raw_sources = entry.get("sources") or []
        if not isinstance(raw_sources, list) or len(raw_sources) > 16:
            raise MarketplaceError(f"package entry {index} sources are invalid")
        sources = tuple(
            _location(value, f"package entry {index} source {source_index}").__dict__
            for source_index, value in enumerate(raw_sources)
        )
        artifact = entry.get("artifact")
        if artifact is not None:
            artifact = validate_artifact(artifact, f"package entry {index} artifact")
            if not manifest_sha256:
                raise MarketplaceError(
                    f"package entry {index} artifact needs an approved manifest SHA-256"
                )
        package_entry = PackageEntry(
            package_id=package_id,
            version=version,
            marketplace_id=entry_marketplace_id,
            manifest=manifest_path,
            manifest_sha256=manifest_sha256,
            sources=sources,
            artifact=artifact,
        )
        if validate_packages:
            local_manifest_path = catalog_path.parent / manifest_path
            manifest = load_manifest(local_manifest_path)
            if manifest["id"] != package_id or manifest["version"] != version:
                raise MarketplaceError(
                    f"package entry {package_id} does not match its manifest identity"
                )
            if (
                manifest_sha256
                and hashlib.sha256(local_manifest_path.read_bytes()).hexdigest()
                != manifest_sha256
            ):
                raise MarketplaceError(
                    f"package entry {package_id} manifest SHA-256 does not match"
                )
        entries.append(package_entry)
        if validate_packages:
            manifests[package_id] = manifest
    if validate_packages:
        validate_dependencies(manifests)
    return Catalog(catalog_path, market_id, name, locations, tuple(entries))


def _read_json_object(path: Path | str, label: str) -> dict:
    try:
        raw = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise MarketplaceError(f"could not read {label} {path}: {error}") from error
    root = _object(raw, label)
    if root.get("schemaVersion") != 1:
        raise MarketplaceError(f"{label} must use schemaVersion 1")
    return root


def _text_field(entry: dict, key: str, label: str, maximum: int,
                required: bool = True) -> str:
    value = entry.get(key, "")
    if not isinstance(value, str) or (required and not value.strip()):
        raise MarketplaceError(f"{label} {key} must be a non-empty string")
    if len(value) > maximum:
        raise MarketplaceError(f"{label} {key} is longer than {maximum} characters")
    return value


def _package_list(value: object, label: str, package_ids: set[str]) -> list[str]:
    if not isinstance(value, list) or not 1 <= len(value) <= 128:
        raise MarketplaceError(f"{label} must list 1-128 packages")
    if len(set(value)) != len(value):
        raise MarketplaceError(f"{label} lists a package twice")
    unknown = [item for item in value if item not in package_ids]
    if unknown:
        raise MarketplaceError(f"{label} names packages missing from the catalog: {unknown}")
    return list(value)


def load_packs(path: Path | str, package_ids: Iterable[str]) -> list[dict]:
    root = _read_json_object(path, "packs file")
    if set(root) - {"$schema", "schemaVersion", "packs"}:
        raise MarketplaceError("packs file has unsupported fields")
    raw_packs = root.get("packs")
    if not isinstance(raw_packs, list) or not 1 <= len(raw_packs) <= 32:
        raise MarketplaceError("packs file must list 1-32 packs")
    known = set(package_ids)
    packs: list[dict] = []
    seen: set[str] = set()
    for index, value in enumerate(raw_packs):
        entry = _object(value, f"pack {index}")
        unknown = set(entry) - {"id", "name", "tagline", "summary", "packages"}
        if unknown:
            raise MarketplaceError(f"pack {index} has unsupported fields: {sorted(unknown)}")
        pack_id = str(entry.get("id") or "")
        if not PACKAGE_ID_RE.fullmatch(pack_id) or len(pack_id) > 64:
            raise MarketplaceError(f"pack {index} id is invalid")
        if pack_id in seen or pack_id in known:
            raise MarketplaceError(f"pack id {pack_id} is already taken")
        seen.add(pack_id)
        label = f"pack {pack_id}"
        packages = entry.get("packages")
        if packages != "*":
            packages = _package_list(packages, f"{label} packages", known)
        packs.append({
            "id": pack_id,
            "name": _text_field(entry, "name", label, 60),
            "tagline": _text_field(entry, "tagline", label, 120),
            "summary": _text_field(entry, "summary", label, 400),
            "packages": packages,
        })
    return packs


def load_guides(path: Path | str, package_ids: Iterable[str],
                pack_ids: Iterable[str]) -> list[dict]:
    root = _read_json_object(path, "guides file")
    if set(root) - {"$schema", "schemaVersion", "guides"}:
        raise MarketplaceError("guides file has unsupported fields")
    raw_guides = root.get("guides")
    if not isinstance(raw_guides, list) or not 1 <= len(raw_guides) <= 32:
        raise MarketplaceError("guides file must list 1-32 guides")
    known = set(package_ids)
    packs = set(pack_ids)
    guides: list[dict] = []
    seen: set[str] = set()
    for index, value in enumerate(raw_guides):
        entry = _object(value, f"guide {index}")
        unknown = set(entry) - {
            "id", "title", "from", "summary", "pack", "packages", "steps", "links"
        }
        if unknown:
            raise MarketplaceError(f"guide {index} has unsupported fields: {sorted(unknown)}")
        guide_id = str(entry.get("id") or "")
        if not PACKAGE_ID_RE.fullmatch(guide_id) or len(guide_id) > 64:
            raise MarketplaceError(f"guide {index} id is invalid")
        if guide_id in seen:
            raise MarketplaceError(f"duplicate guide id: {guide_id}")
        seen.add(guide_id)
        label = f"guide {guide_id}"
        pack = entry.get("pack", "")
        if pack and pack not in packs:
            raise MarketplaceError(f"{label} names an unknown pack: {pack}")
        packages = entry.get("packages", [])
        if packages:
            packages = _package_list(packages, f"{label} packages", known)
        if not pack and not packages:
            raise MarketplaceError(f"{label} needs a pack or packages to install")
        steps = entry.get("steps")
        if not isinstance(steps, list) or not 2 <= len(steps) <= 12:
            raise MarketplaceError(f"{label} needs 2-12 steps")
        for step in steps:
            if not isinstance(step, str) or not step.strip() or len(step) > 400:
                raise MarketplaceError(f"{label} steps must be 1-400 characters")
        links = entry.get("links", [])
        if not isinstance(links, list) or len(links) > 6:
            raise MarketplaceError(f"{label} links must be a list of up to 6")
        clean_links = []
        for link in links:
            link = _object(link, f"{label} link")
            if set(link) - {"label", "url"}:
                raise MarketplaceError(f"{label} link has unsupported fields")
            link_label = _text_field(link, "label", f"{label} link", 60)
            url = _text_field(link, "url", f"{label} link", 300)
            parsed = urlparse(url)
            if parsed.scheme != "https" or not parsed.hostname:
                raise MarketplaceError(f"{label} link {url} must be an https URL")
            clean_links.append({"label": link_label, "url": url})
        guides.append({
            "id": guide_id,
            "title": _text_field(entry, "title", label, 80),
            "from": _text_field(entry, "from", label, 60),
            "summary": _text_field(entry, "summary", label, 400),
            "pack": pack,
            "packages": packages,
            "steps": list(steps),
            "links": clean_links,
        })
    return guides


def load_onboarding(catalog: Catalog) -> tuple[list[dict], list[dict]]:
    ids = [entry.package_id for entry in catalog.packages]
    packs_path = catalog.root / "packs.json"
    guides_path = catalog.root / "guides.json"
    packs = load_packs(packs_path, ids) if packs_path.is_file() else []
    guides = (
        load_guides(guides_path, ids, [pack["id"] for pack in packs])
        if guides_path.is_file() else []
    )
    return packs, guides


def validate_artifact(value: object, label: str = "artifact") -> dict:
    artifact = _object(value, label)
    unknown = set(artifact) - {"sha256", "size", "locations"}
    if unknown:
        raise MarketplaceError(f"{label} has unsupported fields: {sorted(unknown)}")
    digest = str(artifact.get("sha256") or "")
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise MarketplaceError(f"{label} needs a lowercase SHA-256")
    size = artifact.get("size")
    if size is not None and (not isinstance(size, int) or size <= 0):
        raise MarketplaceError(f"{label} size must be a positive integer")
    raw_locations = artifact.get("locations")
    if not isinstance(raw_locations, list) or not 1 <= len(raw_locations) <= 16:
        raise MarketplaceError(f"{label} needs 1-16 locations")
    locations = [
        _location(item, f"{label} location {index}")
        for index, item in enumerate(raw_locations)
    ]
    if any(location.transport == "git" for location in locations):
        raise MarketplaceError(f"{label} locations must be local or HTTPS archives")
    return {
        "sha256": digest,
        **({"size": size} if size is not None else {}),
        "locations": [location.__dict__ for location in locations],
    }


def _manifest_reference(package_root: Path, value: object, label: str) -> Path:
    relative = safe_relative_path(value, label)
    target = (package_root / relative).resolve()
    try:
        target.relative_to(package_root.resolve())
    except ValueError as error:
        raise MarketplaceError(f"{label} escapes its package") from error
    if not target.is_file():
        raise MarketplaceError(f"{label} does not exist: {relative}")
    return target


def load_manifest(path: Path | str) -> dict:
    manifest_path = Path(path).resolve()
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise MarketplaceError(f"could not read manifest {manifest_path}: {error}") from error
    root = _object(manifest, "package manifest")
    package_id = str(root.get("id") or "")
    if root.get("schemaVersion") != 1:
        raise MarketplaceError(f"package {package_id or manifest_path.name} has unsupported schema")
    if not PACKAGE_ID_RE.fullmatch(package_id) or len(package_id) > 96:
        raise MarketplaceError("package id is invalid")
    version = str(root.get("version") or "")
    if not VERSION_RE.fullmatch(version):
        raise MarketplaceError(f"package {package_id} version is invalid")
    name = str(root.get("name") or "")
    summary = str(root.get("summary") or "")
    creator = _object(root.get("creator"), f"package {package_id} creator")
    license_block = _object(root.get("license"), f"package {package_id} license")
    price = _object(root.get("price"), f"package {package_id} price")
    if (
        not 1 <= len(name) <= 80
        or not 1 <= len(summary) <= 240
        or root.get("type") not in PACKAGE_TYPES
        or not str(creator.get("id") or "").startswith("did:forkmesh:")
        or not re.fullmatch(r"[a-z0-9][a-z0-9-]*", str(creator.get("handle") or ""))
        or not 1 <= len(str(creator.get("displayName") or "")) <= 80
        or not isinstance(creator.get("verified"), bool)
        or license_block.get("model") not in LICENSE_MODELS
        or not 1 <= len(str(license_block.get("spdx") or "")) <= 64
        or license_block.get("creatorRetainsOwnership") is not True
        or isinstance(price.get("amount"), bool)
        or not isinstance(price.get("amount"), (int, float))
        or price.get("amount") < 0
        or not re.fullmatch(r"[A-Z]{3}", str(price.get("currency") or ""))
    ):
        raise MarketplaceError(f"package {package_id} metadata is invalid")
    package_root = manifest_path.parent
    screenshots = root.get("screenshots")
    if not isinstance(screenshots, list) or not 1 <= len(screenshots) <= 8:
        raise MarketplaceError(f"package {package_id} needs 1-8 screenshots")
    for index, screenshot in enumerate(screenshots):
        raw = _object(screenshot, f"package {package_id} screenshot {index}")
        relative = safe_relative_path(raw.get("path"), "screenshot path")
        if not relative.startswith("preview/"):
            raise MarketplaceError(f"package {package_id} screenshot must be under preview/")
        _manifest_reference(package_root, relative, "screenshot")
        if not str(raw.get("alt") or "").strip():
            raise MarketplaceError(f"package {package_id} screenshot alt text is required")
    entrypoints = _object(root.get("entrypoints"), f"package {package_id} entrypoints")
    if not entrypoints or not str(entrypoints.get("settings") or ""):
        raise MarketplaceError(f"package {package_id} needs an entrypoint and settings page")
    for key, value in entrypoints.items():
        if not re.fullmatch(r"[a-z][a-z0-9.-]{0,63}", str(key)):
            raise MarketplaceError(f"package {package_id} entrypoint name is invalid")
        text = str(value or "")
        if not text or len(text) > 160:
            raise MarketplaceError(f"package {package_id} entrypoint is invalid")
        if text.startswith(("content/", "migrations/")):
            _manifest_reference(package_root, text, f"{key} entrypoint")
    contributions = root.get("contributions") or []
    if not isinstance(contributions, list) or len(contributions) > 64:
        raise MarketplaceError(f"package {package_id} contributions are invalid")
    contribution_ids: set[str] = set()
    for index, contribution in enumerate(contributions):
        raw = _object(
            contribution, f"package {package_id} contribution {index}"
        )
        unknown = set(raw) - {"id", "socket", "entrypoint", "config"}
        if unknown:
            raise MarketplaceError(
                f"package {package_id} contribution has unsupported fields: "
                f"{sorted(unknown)}"
            )
        contribution_id = str(raw.get("id") or "default")
        socket = str(raw.get("socket") or "")
        entrypoint_name = str(raw.get("entrypoint") or "")
        config = raw.get("config") or {}
        if (
            not re.fullmatch(r"[a-z][a-z0-9.-]{0,63}", contribution_id)
            or contribution_id in contribution_ids
            or not re.fullmatch(
                r"[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+", socket
            )
            or len(socket) > 96
            or entrypoint_name not in entrypoints
            or not isinstance(config, dict)
        ):
            raise MarketplaceError(
                f"package {package_id} contribution {index} is invalid"
            )
        contribution_ids.add(contribution_id)
        target = str(entrypoints[entrypoint_name])
        if not target.startswith("content/"):
            raise MarketplaceError(
                f"package {package_id} contribution entrypoint must be package content"
            )
    permissions = root.get("permissions")
    if not isinstance(permissions, list) or len(permissions) > 32:
        raise MarketplaceError(f"package {package_id} permissions are invalid")
    permission_ids = set()
    for permission in permissions:
        raw = _object(permission, f"package {package_id} permission")
        permission_id = str(raw.get("id") or "")
        if (
            not re.fullmatch(r"[a-z][a-z0-9.]*", permission_id)
            or permission_id in permission_ids
            or not 1 <= len(str(raw.get("label") or "")) <= 120
            or raw.get("risk") not in {"low", "sensitive", "high"}
        ):
            raise MarketplaceError(f"package {package_id} permissions are invalid")
        permission_ids.add(permission_id)
    validate_settings(root.get("settings"), package_id)
    install = root.get("install") or {}
    if not isinstance(install, dict) or install.get("sync", "account") != "account":
        raise MarketplaceError(f"package {package_id} install block is invalid")
    installed_before = install.get("installedBefore")
    if installed_before is not None and (
        not isinstance(installed_before, str)
        or not INSTALLED_BEFORE_RE.fullmatch(installed_before)
        or install.get("default") is not False
    ):
        raise MarketplaceError(f"package {package_id} install.installedBefore is invalid")
    places = install.get("places")
    if places is not None and (
        not isinstance(places, list)
        or not places
        or len(set(places)) != len(places)
        or not set(places) <= {"mobile", "desktop", "web"}
    ):
        raise MarketplaceError(f"package {package_id} install.places is invalid")
    dependencies = root.get("dependencies") or []
    if not isinstance(dependencies, list) or len(dependencies) > 64:
        raise MarketplaceError(f"package {package_id} dependencies are invalid")
    dependency_ids: set[str] = set()
    for dependency in dependencies:
        raw = _object(dependency, f"package {package_id} dependency")
        dependency_id = str(raw.get("id") or "")
        if (
            not PACKAGE_ID_RE.fullmatch(dependency_id)
            or dependency_id == package_id
            or dependency_id in dependency_ids
            or not str(raw.get("version") or "")
        ):
            raise MarketplaceError(f"package {package_id} dependencies are invalid")
        dependency_ids.add(dependency_id)
    lifecycle = _object(root.get("lifecycle"), f"package {package_id} lifecycle")
    migrations = lifecycle.get("migrations")
    if not isinstance(migrations, list) or len(migrations) > 128:
        raise MarketplaceError(f"package {package_id} lifecycle migrations are invalid")
    migration_ids: set[str] = set()
    for index, migration in enumerate(migrations):
        raw = _object(migration, f"package {package_id} migration {index}")
        unknown = set(raw) - {"id", "database", "up", "down"}
        if unknown:
            raise MarketplaceError(
                f"package {package_id} migration has unsupported fields: {sorted(unknown)}"
            )
        migration_id = str(raw.get("id") or "")
        if not MIGRATION_ID_RE.fullmatch(migration_id) or migration_id in migration_ids:
            raise MarketplaceError(f"package {package_id} migration id is invalid or duplicated")
        migration_ids.add(migration_id)
        database = str(raw.get("database") or "device")
        if database not in {"device", "organization"}:
            raise MarketplaceError(f"package {package_id} migration database is invalid")
        up = _manifest_reference(package_root, raw.get("up"), "up migration")
        down = _manifest_reference(package_root, raw.get("down"), "down migration")
        if up == down:
            raise MarketplaceError(f"package {package_id} migration up/down must differ")
        validate_migration_sql(up, package_id, migration_id, "up")
        validate_migration_sql(down, package_id, migration_id, "down")
    validate_service(root.get("service"), package_root, package_id, permission_ids)
    return root


SERVICE_NAME_RE = re.compile(r"[a-z][A-Za-z0-9_]{0,63}")
SERVICE_INTERPRETERS = {"python3", "sh", "bash", "node"}
SERVICE_FIELDS = {
    "api", "exec", "interpreter", "start", "methods", "events",
    "remoteMethods", "remoteEvents", "idleExitSeconds", "memoryMB",
}


def validate_service(
    value: object, package_root: Path, package_id: str, permission_ids: set[str]
) -> dict | None:
    if value is None:
        return None
    service = _object(value, f"package {package_id} service")
    unknown = set(service) - SERVICE_FIELDS
    if unknown:
        raise MarketplaceError(
            f"package {package_id} service has unsupported fields: {sorted(unknown)}"
        )
    if service.get("api") != 1:
        raise MarketplaceError(f"package {package_id} service api must be 1")
    executable = str(service.get("exec") or "")
    if not executable.startswith("content/") or "{" in executable:
        raise MarketplaceError(
            f"package {package_id} service exec must be a file under content/"
        )
    _manifest_reference(package_root, safe_relative_path(executable, "service exec"), "service exec")
    interpreter = service.get("interpreter")
    if interpreter is not None and interpreter not in SERVICE_INTERPRETERS:
        raise MarketplaceError(f"package {package_id} service interpreter is not allowed")
    if service.get("start", "lazy") not in {"lazy", "boot"}:
        raise MarketplaceError(f"package {package_id} service start must be lazy or boot")
    lists = {}
    for key in ("methods", "events", "remoteMethods", "remoteEvents"):
        items = service.get(key) or []
        if (
            not isinstance(items, list)
            or len(items) > 64
            or len(set(items)) != len(items)
            or not all(isinstance(item, str) and SERVICE_NAME_RE.fullmatch(item) for item in items)
        ):
            raise MarketplaceError(f"package {package_id} service {key} are invalid")
        lists[key] = set(items)
    if not lists["methods"]:
        raise MarketplaceError(f"package {package_id} service needs at least one method")
    if not lists["remoteMethods"] <= lists["methods"] or not lists["remoteEvents"] <= lists["events"]:
        raise MarketplaceError(
            f"package {package_id} service remote methods and events must be declared ones"
        )
    for key, low, high in (("idleExitSeconds", 10, 86400), ("memoryMB", 64, 4096)):
        number = service.get(key)
        if number is not None and (
            isinstance(number, bool) or not isinstance(number, int) or not low <= number <= high
        ):
            raise MarketplaceError(f"package {package_id} service {key} must be {low}..{high}")
    unknown_permissions = {
        permission for permission in permission_ids if permission.startswith("service.")
    } - {"service.network"}
    if unknown_permissions:
        raise MarketplaceError(
            f"package {package_id} requests unknown service permissions: {sorted(unknown_permissions)}"
        )
    return service


def validate_settings(value: object, package_id: str) -> list[dict]:
    if value is None:
        return []
    if not isinstance(value, list) or len(value) > 16:
        raise MarketplaceError(f"package {package_id} settings are invalid")
    keys: set[str] = set()
    for index, setting in enumerate(value):
        raw = _object(setting, f"package {package_id} setting {index}")
        key = str(raw.get("key") or "")
        kind = raw.get("type")
        label = str(raw.get("label") or "")
        default = raw.get("default")
        options = raw.get("options")
        if (
            set(raw) - {"key", "type", "label", "default", "options"}
            or not SETTING_KEY_RE.fullmatch(key)
            or key in keys
            or not 1 <= len(label) <= 80
        ):
            raise MarketplaceError(f"package {package_id} setting {index} is invalid")
        keys.add(key)
        if kind == "boolean":
            if not isinstance(default, bool) or options is not None:
                raise MarketplaceError(f"package {package_id} setting {key} is invalid")
            continue
        if kind != "choice" or not isinstance(options, list) or not 2 <= len(options) <= 12:
            raise MarketplaceError(f"package {package_id} setting {key} is invalid")
        values: set[str] = set()
        for option in options:
            raw_option = _object(option, f"package {package_id} setting {key} option")
            option_value = str(raw_option.get("value") or "")
            if (
                set(raw_option) - {"value", "label"}
                or not SETTING_VALUE_RE.fullmatch(option_value)
                or option_value in values
                or not 1 <= len(str(raw_option.get("label") or "")) <= 60
            ):
                raise MarketplaceError(f"package {package_id} setting {key} option is invalid")
            values.add(option_value)
        if not isinstance(default, str) or default not in values:
            raise MarketplaceError(f"package {package_id} setting {key} default is invalid")
    return value


def validate_dependencies(manifests: dict[str, dict]) -> None:
    graph = {
        package_id: [str(row.get("id") or "") for row in manifest.get("dependencies") or []]
        for package_id, manifest in manifests.items()
    }
    for package_id, needs in graph.items():
        synced = (manifests[package_id].get("install") or {}).get("sync") == "account"
        for dependency in needs:
            if dependency not in graph:
                raise MarketplaceError(
                    f"package {package_id} depends on {dependency}, which is not in the catalog"
                )
            if synced != ((manifests[dependency].get("install") or {}).get("sync") == "account"):
                raise MarketplaceError(
                    f"package {package_id} and its dependency {dependency} must share install.sync"
                )
    state: dict[str, int] = {}

    def visit(package_id: str, trail: tuple[str, ...]) -> None:
        if state.get(package_id) == 2:
            return
        if state.get(package_id) == 1:
            raise MarketplaceError(
                "package dependency cycle: " + " -> ".join(trail + (package_id,))
            )
        state[package_id] = 1
        for dependency in graph[package_id]:
            visit(dependency, trail + (package_id,))
        state[package_id] = 2

    for package_id in sorted(graph):
        visit(package_id, ())


def validate_migration_sql(path: Path, package_id: str, migration_id: str, direction: str) -> str:
    try:
        sql = path.read_text(encoding="utf-8")
    except OSError as error:
        raise MarketplaceError(f"could not read {direction} migration: {error}") from error
    if not sql.strip():
        raise MarketplaceError(f"package {package_id} migration {migration_id} {direction} is empty")
    if TRANSACTION_SQL_RE.search(sql):
        raise MarketplaceError(
            f"package {package_id} migration {migration_id} controls transactions; the host owns the transaction"
        )
    return sql


def manifest_entries(catalog: Catalog) -> Iterable[tuple[PackageEntry, dict]]:
    for entry in catalog.packages:
        yield entry, load_manifest(catalog.root / entry.manifest)


def _run_git(arguments: list[str], cwd: Path | None = None) -> None:
    try:
        result = subprocess.run(
            ["git", *arguments], cwd=cwd, text=True, capture_output=True, check=False
        )
    except OSError as error:
        raise MarketplaceError(f"could not run Git: {error}") from error
    if result.returncode:
        detail = (result.stderr or result.stdout).strip().splitlines()
        raise MarketplaceError(detail[-1] if detail else "Git source failed")


def materialize_catalog(location: Location, destination: Path, base: Path) -> Path:
    """Materialize one catalog location. Callers provide ordered failover."""
    destination = destination.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    if location.transport == "local":
        root = Path(location.url)
        if not root.is_absolute():
            root = (base / root).resolve()
        source = root / location.catalog
        load_catalog(source)
        return source
    if location.transport == "https":
        request = Request(location.url, headers={"User-Agent": "ForkMesh-Market/1"})
        try:
            with urlopen(request, timeout=30) as response:
                body = response.read(2 * 1024 * 1024 + 1)
        except OSError as error:
            raise MarketplaceError(f"HTTPS catalog failed: {error}") from error
        if len(body) > 2 * 1024 * 1024:
            raise MarketplaceError("HTTPS catalog is too large")
        destination.mkdir(parents=True, exist_ok=True)
        catalog_path = destination / "catalog.json"
        catalog_path.write_bytes(body)
        load_catalog(catalog_path, validate_packages=False)
        return catalog_path
    temporary = Path(tempfile.mkdtemp(prefix="forkmesh-market-", dir=destination.parent))
    try:
        clone = ["clone", "--depth", "1", "--no-tags"]
        if location.ref:
            clone += ["--branch", location.ref]
        clone += [location.url, str(temporary)]
        _run_git(clone)
        source = temporary / location.catalog
        load_catalog(source)
        if destination.exists():
            if destination.is_symlink() or not (destination / location.catalog).is_file():
                raise MarketplaceError(
                    f"refusing to replace {destination}: it is not a materialized marketplace catalog"
                )
            shutil.rmtree(destination)
        os.replace(temporary, destination)
        return destination / location.catalog
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def sync_marketplace(locations: Iterable[Location], destination: Path, base: Path) -> Path:
    errors = []
    for location in locations:
        try:
            return materialize_catalog(location, destination, base)
        except MarketplaceError as error:
            errors.append(f"{location.url}: {error}")
    raise MarketplaceError("all marketplace locations failed: " + "; ".join(errors))


def _download_artifact(artifact: dict, destination: Path, base: Path) -> Path:
    errors = []
    expected = artifact["sha256"]
    for raw in artifact["locations"]:
        location = Location(**raw)
        try:
            if location.transport == "local":
                source = Path(location.url)
                if not source.is_absolute():
                    source = (base / source).resolve()
                body = source.read_bytes()
            else:
                request = Request(location.url, headers={"User-Agent": "ForkMesh-Market/1"})
                with urlopen(request, timeout=60) as response:
                    body = response.read(MAX_ARCHIVE_BYTES + 1)
            if len(body) > MAX_ARCHIVE_BYTES:
                raise MarketplaceError("package archive is too large")
            if artifact.get("size") is not None and len(body) != artifact["size"]:
                raise MarketplaceError("package archive size does not match catalog")
            if hashlib.sha256(body).hexdigest() != expected:
                raise MarketplaceError("package archive SHA-256 does not match catalog")
            destination.write_bytes(body)
            return destination
        except (OSError, MarketplaceError) as error:
            errors.append(f"{location.url}: {error}")
    raise MarketplaceError("all package artifact locations failed: " + "; ".join(errors))


def _extract_archive(archive: Path, destination: Path) -> Path:
    try:
        package = zipfile.ZipFile(archive)
    except (OSError, zipfile.BadZipFile) as error:
        raise MarketplaceError(f"invalid .fmpkg archive: {error}") from error
    with package:
        infos = package.infolist()
        if not infos or len(infos) > MAX_ARCHIVE_FILES:
            raise MarketplaceError("package archive has an invalid file count")
        total = 0
        for info in infos:
            relative = safe_relative_path(info.filename.rstrip("/"), "archive member")
            total += info.file_size
            if total > MAX_ARCHIVE_BYTES:
                raise MarketplaceError("expanded package archive is too large")
            # Unix symlinks could point out of the package after extraction.
            if (info.external_attr >> 16) & 0o170000 == 0o120000:
                raise MarketplaceError(f"package archive contains symlink: {relative}")
        package.extractall(destination)
    manifest = destination / "manifest.json"
    if manifest.is_file():
        return destination
    children = [child for child in destination.iterdir() if child.is_dir()]
    if len(children) == 1 and (children[0] / "manifest.json").is_file():
        return children[0]
    raise MarketplaceError("package archive does not contain manifest.json at one root")


def package_source(catalog: Catalog, entry: PackageEntry, temporary: Path) -> Path:
    local_manifest = catalog.root / entry.manifest
    if local_manifest.is_file():
        load_manifest(local_manifest)
        return local_manifest.parent
    if entry.artifact is None:
        raise MarketplaceError(f"package {entry.manifest} is unavailable and has no artifact")
    archive = _download_artifact(entry.artifact, temporary / "package.fmpkg", catalog.root)
    return _extract_archive(archive, temporary / "expanded")


def _migration_rows(
    connection: sqlite3.Connection, package_id: str, scope: str
) -> list[str]:
    connection.execute(
        "CREATE TABLE IF NOT EXISTS forkmesh_plugin_migrations ("
        "package_id TEXT NOT NULL, scope TEXT NOT NULL, "
        "migration_id TEXT NOT NULL, applied_at TEXT NOT NULL "
        "DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(package_id,scope,migration_id))"
    )
    return [
        str(row[0])
        for row in connection.execute(
            "SELECT migration_id FROM forkmesh_plugin_migrations "
            "WHERE package_id=? AND scope=? ORDER BY migration_id",
            (package_id, scope),
        )
    ]


def _apply_migrations(
    connection: sqlite3.Connection, package_root: Path, manifest: dict, *,
    down: bool, scope: str = "device"
) -> None:
    if scope not in {"device", "organization"}:
        raise MarketplaceError(f"unsupported migration database scope: {scope}")
    package_id = manifest["id"]
    migrations = list((manifest["lifecycle"] or {}).get("migrations") or [])
    scoped = [
        item for item in migrations
        if item.get("database", "device") == scope
    ]
    applied = set(_migration_rows(connection, package_id, scope))
    selected = [item for item in scoped if (item["id"] in applied) == down]
    if down:
        selected.reverse()
    statements = []
    for item in selected:
        direction = "down" if down else "up"
        path = package_root / safe_relative_path(item[direction], f"{direction} migration")
        statements.append(
            validate_migration_sql(path, package_id, item["id"], direction)
        )
    if not selected:
        return
    transaction = ["BEGIN IMMEDIATE;"]
    for item, sql in zip(selected, statements):
        transaction.append(sql.rstrip().rstrip(";") + ";")
        if down:
            transaction.append(
                "DELETE FROM forkmesh_plugin_migrations "
                f"WHERE package_id='{package_id}' AND scope='{scope}' "
                f"AND migration_id='{item['id']}';"
            )
        else:
            transaction.append(
                "INSERT INTO forkmesh_plugin_migrations"
                "(package_id,scope,migration_id) "
                f"VALUES('{package_id}','{scope}','{item['id']}');"
            )
    transaction.append("COMMIT;")
    try:
        # sqlite3.execute() cannot accept multiple statements. Splitting SQL
        # correctly requires SQLite's parser, so execute one host-owned script.
        # Package and migration ids are restricted to the safe regexes above.
        connection.executescript("\n".join(transaction))
    except Exception:
        connection.rollback()
        raise


def _installed_catalog(install_root: Path, marketplace_id: str) -> dict:
    entries = []
    packages_root = install_root / "packages"
    if packages_root.is_dir():
        for manifest in sorted(packages_root.glob("*/manifest.json")):
            package = load_manifest(manifest)
            provenance_path = manifest.parent / ".forkmesh-install.json"
            try:
                provenance = json.loads(
                    provenance_path.read_text(encoding="utf-8")
                )
            except (OSError, json.JSONDecodeError):
                provenance = {}
            source_marketplace = str(
                provenance.get("marketplace") or "installed"
            )
            if not MARKETPLACE_ID_RE.fullmatch(source_marketplace):
                source_marketplace = "installed"
            entries.append({
                "id": package["id"],
                "version": package["version"],
                "marketplace": source_marketplace,
                "manifest": manifest.relative_to(install_root).as_posix(),
                "manifestSha256": hashlib.sha256(manifest.read_bytes()).hexdigest(),
            })
    return {
        "$schema": "https://forkmesh.com/schemas/marketplace-catalog-v2.json",
        "schemaVersion": 2,
        "marketplace": {
            "id": marketplace_id,
            "name": "Installed packages",
            "locations": [
                {"transport": "local", "url": ".", "catalog": "catalog.json"}
            ],
        },
        "packages": entries,
    }


def _write_installed_catalog(install_root: Path, marketplace_id: str) -> None:
    install_root.mkdir(parents=True, exist_ok=True)
    target = install_root / "catalog.json"
    temporary = install_root / ".catalog.json.tmp"
    temporary.write_text(
        json.dumps(_installed_catalog(install_root, marketplace_id), indent=2) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, target)


def install_package(
    catalog: Catalog,
    package_id: str,
    install_root: Path,
    database: Path | None = None,
    database_scope: str = "device",
) -> Path:
    if database_scope not in {"device", "organization"}:
        raise MarketplaceError(
            f"unsupported migration database scope: {database_scope}"
        )
    matches = [entry for entry in catalog.packages if entry.package_id == package_id]
    if len(matches) != 1:
        raise MarketplaceError(f"unknown or ambiguous package: {package_id}")
    entry = matches[0]
    install_root = install_root.resolve()
    packages_root = install_root / "packages"
    packages_root.mkdir(parents=True, exist_ok=True)
    target = packages_root / package_id
    if target.exists():
        raise MarketplaceError(
            f"package is already installed: {package_id}; uninstall it before replacing"
        )
    staging = Path(tempfile.mkdtemp(prefix=f".{package_id}-", dir=packages_root))
    migrations_applied = False
    try:
        source = package_source(catalog, entry, staging)
        if entry.manifest_sha256 and hashlib.sha256(
            (source / "manifest.json").read_bytes()
        ).hexdigest() != entry.manifest_sha256:
            raise MarketplaceError(
                "package manifest SHA-256 does not match its approved catalog entry"
            )
        manifest = load_manifest(source / "manifest.json")
        if manifest["id"] != package_id or manifest["version"] != entry.version:
            raise MarketplaceError("catalog package identity does not match its artifact")
        scoped_migrations = [
            item for item in (manifest.get("lifecycle") or {}).get("migrations", [])
            if item.get("database", "device") == database_scope
        ]
        if scoped_migrations and database is None:
            raise MarketplaceError(
                "install needs the package database to run up migrations"
            )
        payload = staging / "payload"
        if source != payload:
            linked = [path for path in source.rglob("*") if path.is_symlink()]
            if linked:
                raise MarketplaceError(
                    "package source contains symlink: "
                    + linked[0].relative_to(source).as_posix()
                )
            shutil.copytree(source, payload)
        manifest = load_manifest(payload / "manifest.json")
        (payload / ".forkmesh-install.json").write_text(
            json.dumps({
                "schemaVersion": 1,
                "marketplace": entry.marketplace_id,
                "package": package_id,
                "version": entry.version,
            }, sort_keys=True, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        os.replace(payload, target)
        if database is not None:
            with sqlite3.connect(database) as connection:
                _apply_migrations(
                    connection, target, manifest,
                    down=False, scope=database_scope)
            migrations_applied = True
        _write_installed_catalog(install_root, "installed")
        return target
    except Exception:
        if migrations_applied and database is not None and target.exists():
            with sqlite3.connect(database) as connection:
                _apply_migrations(
                    connection, target, manifest,
                    down=True, scope=database_scope)
        if target.exists():
            shutil.rmtree(target)
        raise
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def uninstall_package(
    package_id: str,
    install_root: Path,
    database: Path | None = None,
    database_scope: str = "device",
) -> None:
    if database_scope not in {"device", "organization"}:
        raise MarketplaceError(
            f"unsupported migration database scope: {database_scope}"
        )
    install_root = install_root.resolve()
    target = install_root / "packages" / package_id
    manifest_path = target / "manifest.json"
    if not manifest_path.is_file():
        raise MarketplaceError(f"package is not installed: {package_id}")
    manifest = load_manifest(manifest_path)
    if manifest["id"] != package_id:
        raise MarketplaceError("installed package directory does not match its manifest")
    migrations = [
        item for item in (manifest.get("lifecycle") or {}).get("migrations", [])
        if item.get("database", "device") == database_scope
    ]
    if migrations and database is None:
        raise MarketplaceError("uninstall needs the package database to run down migrations")
    # Make removal recoverable until every down migration commits. Runtime
    # discovery stops seeing the package immediately, while a failed rollback
    # restores its exact directory rather than leaving installed code whose
    # schema was already removed.
    quarantine = install_root / "packages" / f".{package_id}.uninstalling"
    if quarantine.exists():
        raise MarketplaceError(f"stale uninstall quarantine exists: {quarantine}")
    os.replace(target, quarantine)
    migrations_removed = False
    try:
        if database is not None:
            with sqlite3.connect(database) as connection:
                _apply_migrations(
                    connection, quarantine, manifest,
                    down=True, scope=database_scope)
            migrations_removed = True
        _write_installed_catalog(install_root, "installed")
        shutil.rmtree(quarantine)
    except Exception:
        if migrations_removed and database is not None and quarantine.exists():
            with sqlite3.connect(database) as connection:
                _apply_migrations(
                    connection, quarantine, manifest,
                    down=False, scope=database_scope)
        if quarantine.exists() and not target.exists():
            os.replace(quarantine, target)
        _write_installed_catalog(install_root, "installed")
        raise

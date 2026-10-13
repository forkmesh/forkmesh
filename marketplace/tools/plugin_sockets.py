"""Discover package contributions without knowing individual plugins.

Packages declare named sockets in ``manifest.json``.  A host asks for the
socket names it implements and receives package-owned entrypoints plus opaque
configuration.  Adding a package or inventing a socket never requires adding a
package id or package type to application code.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from marketplace_lib import Catalog, load_catalog, manifest_entries, safe_relative_path


@dataclass(frozen=True)
class Contribution:
    package_id: str
    contribution_id: str
    socket: str
    entrypoint: Path
    config: dict
    manifest: dict


def discover(
    catalog_path: Path | str, sockets: Iterable[str] | None = None
) -> tuple[Contribution, ...]:
    """Return validated contributions, optionally filtered by socket name."""
    catalog: Catalog = load_catalog(catalog_path)
    accepted = set(sockets) if sockets is not None else None
    found = []
    for entry, manifest in manifest_entries(catalog):
        package_root = (catalog.root / entry.manifest).parent.resolve()
        entrypoints = manifest["entrypoints"]
        for raw in manifest.get("contributions") or []:
            socket = str(raw["socket"])
            if accepted is not None and socket not in accepted:
                continue
            name = str(raw["entrypoint"])
            relative = safe_relative_path(
                entrypoints[name],
                f"package {manifest['id']} {socket} entrypoint",
            )
            source = (package_root / relative).resolve()
            source.relative_to(package_root)
            found.append(Contribution(
                package_id=manifest["id"],
                contribution_id=str(raw.get("id") or "default"),
                socket=socket,
                entrypoint=source,
                config=dict(raw.get("config") or {}),
                manifest=manifest,
            ))
    return tuple(found)
